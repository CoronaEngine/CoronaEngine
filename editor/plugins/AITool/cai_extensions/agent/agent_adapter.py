"""Master Agent — 统一入口，接管 LANChat 全部 AI 能力。

替换 LANChat 原有的两处回调:
  1. AgentRunner 推理 → create_master_agent()     (system, messages) -> str
  2. SummaryService 摘要 → create_summary_agent()  (prompt) -> str

Master Agent 内部能力:
  - 场景编辑 (add/delete/move/modify) → AgentCoordinator
  - 普通聊天 → CAIApp + specialist 上下文
  - 讨论总结 → GroupAgent 共识 + Style Bible 生成
  - 风格巡检 → GroupAgent 定期检查
  - 能力查询 → /help 命令

架构:
  LANChat 群聊消息
    ├─ AgentRunner.run() → MasterAgent(system, messages)
    │   ├─ 场景指令 → Coordinator
    │   ├─ /总结 /summary → GroupAgent.try_summarize()
    │   ├─ /检查 /patrol → GroupAgent.try_patrol()
    │   └─ 其他 → CAIApp 聊天
    │
    └─ SummaryService.compress() → SummaryAgent(prompt)
        └─ 增强版摘要 (含场景上下文)
"""

from __future__ import annotations

import json
import logging
import math
import re
from typing import Any, Callable, Dict, List, Optional

try:
    from plugins.AITool.services.agent_progress_context import (
        agent_progress_sink,
        get_current_progress_sink,
    )
    from plugins.AITool.services.intent_understanding import get_intent_understanding_service
    from plugins.AITool.services.lanchat_scene_runtime import get_lanchat_scene_runtime
    from plugins.AITool.services.workflow_command_policy import (
        DEPRECATED_WORKFLOW_COMMAND_MESSAGE,
        is_deprecated_user_workflow_command,
    )
except Exception:  # noqa: BLE001
    from services.agent_progress_context import (  # type: ignore
        agent_progress_sink,
        get_current_progress_sink,
    )
    from services.intent_understanding import get_intent_understanding_service  # type: ignore
    from services.lanchat_scene_runtime import get_lanchat_scene_runtime  # type: ignore
    from services.workflow_command_policy import (  # type: ignore
        DEPRECATED_WORKFLOW_COMMAND_MESSAGE,
        is_deprecated_user_workflow_command,
    )

logger = logging.getLogger(__name__)

# ═══════════════════════════════════════════════════════════════════════════
# 场景指令检测
# ═══════════════════════════════════════════════════════════════════════════

_SCENE_COMMAND_PATTERNS = [
    r"(?:加个?|添加|放个?|增加|创建|新[增建]|导入).{1,20}",
    r"(?:删[掉除]|移除|去掉|清除).{0,20}$",
    r"(?:把|将).{1,20}(?:移[动到]?|挪[动到]?|搬[到]?|推到?|拉到?)",
    r"(?:往|向)(?:左|右|前|后|上|下).{0,5}(?:移|挪|搬|推|拉)",
    r"(?:放大|缩小|变大|变小|旋转|改成?|调整|修改).{1,20}",
    r"(?:布置|摆放|排列|安排|设计|规划|装饰).{1,30}",
    r"(?:灯光|光照|照明|氛围|环境光|光源).{1,20}",
    r"生成.{0,9}(?:3d|3D|模型|物体|场景|家具|清单|卧室|客厅|厨房|书房|房间|浴室|办公室|餐厅)",
    r"(?:按|根据|依据).{0,10}清单",
    r"(?:组合|搭建).{0,8}(?:场景|房间|卧室|客厅|酒吧)",
    r"@agent\s", r"@场景\s", r"/sc_agent\s",
]

_NON_SCENE_PATTERNS = [
    r"^(?:你好|hello|hi|hey)\b", r"^(?:谢谢|thanks?|thank you)\b",
    r"^(?:什么|怎么|如何|为什么|什么时候)\b", r"^\?\s*$", r"^[。！？\.\!\?]$",
]

_BUILTIN_COMMANDS = [
    r"^/(?:help|能力|list)\b", r"^(?:帮助|你能做什么|有什么能力)\b",
    r"^/(?:总结|summary|summarize)\b", r"^(?:总结一下|来个总结|讨论总结)\b",
    r"^/(?:检查|巡检|patrol|check)\b", r"^(?:巡检一下|风格检查|检查风格)\b",
]


def is_scene_command(text: str) -> bool:
    text = text.strip()
    if not text or len(text) < 2:
        return False
    for pat in _NON_SCENE_PATTERNS:
        if re.match(pat, text):
            return False
    for pat in _SCENE_COMMAND_PATTERNS:
        if re.search(pat, text):
            return True
    return False


def is_builtin_command(text: str) -> Optional[str]:
    """检测内置命令。返回命令类型或 None。

    返回: "help" | "summary" | "patrol" | None
    """
    text = text.strip()
    if re.match(r"^/(?:help|能力|list)\b", text) or re.match(r"^(?:帮助|你能做什么|有什么能力)\b", text):
        return "help"
    if re.match(r"^/(?:总结|summary|summarize)\b", text) or re.match(r"^(?:总结一下|来个总结|讨论总结)\b", text):
        return "summary"
    if re.match(r"^/(?:检查|巡检|patrol|check)\b", text) or re.match(r"^(?:巡检一下|风格检查|检查风格)\b", text):
        return "patrol"
    return None


# ═══════════════════════════════════════════════════════════════════════════
# 开放式意图路由（LLM 判别，替代关键词清单）
# ═══════════════════════════════════════════════════════════════════════════

_COMPOSE_INTENT_SYSTEM_PROMPT = """你是 LANChat 场景生成助手的意图判别器。
判断用户这条消息是否在要求【生成/搭建一个完整的 3D 场景或环境】。

是 compose(返回 true)的例子(不限关键词，任何室内/室外/混合环境都算):
- "生成一个现代客厅" / "做一个赛博朋克酒吧街" / "来个蒙古包草原"
- "我想要一片露营地" / "搭一个海底世界" / "弄个中世纪集市"
- "布置一个北欧风卧室" / "整一个篝火露营的场景"

不是 compose(返回 false)的例子:
- 对【已有场景里单个/少数物体】的增删改移: "把椅子放大" / "删掉那盏灯" / "加一个茶几" / "沙发往左移"
- 闲聊/提问/建议: "你好" / "客厅一般放什么" / "谢谢" / "这个配色好看吗"

只输出 JSON: {"compose": true} 或 {"compose": false}"""


def _llm_is_compose_intent(text: str, timeout: float = 20.0) -> Optional[bool]:
    """LLM 判断是否为整场景生成意图。返回 True/False；失败返回 None（交由调用方兜底）。

    超时：future timeout(20s) 必须 > HTTP request_timeout(18s)，否则 HTTP 还没超时
    就被 future 掐断（曾因 future=8 < http=15 导致 GPT-5.5 在 8s 临界处被误杀掉进 chat）。
    """
    if not text or not text.strip():
        return None
    try:
        from concurrent.futures import ThreadPoolExecutor
        from langchain_core.messages import HumanMessage, SystemMessage
        from Quasar.ai_models.base_pool.registry import get_chat_model

        def _do():
            model = get_chat_model(temperature=0, request_timeout=18.0)
            return model.invoke([
                SystemMessage(content=_COMPOSE_INTENT_SYSTEM_PROMPT),
                HumanMessage(content=text.strip()[:500]),
            ])

        ex = ThreadPoolExecutor(max_workers=1)
        try:
            resp = ex.submit(_do).result(timeout=timeout)
        finally:
            ex.shutdown(wait=False)

        raw = (resp.content if hasattr(resp, "content") else str(resp)).strip()
        if "```" in raw:
            s = raw.find("{")
            e = raw.rfind("}")
            if s != -1 and e != -1:
                raw = raw[s:e + 1]
        data = json.loads(raw)
        result = bool(data.get("compose", False))
        logger.info("[MasterAgent] LLM 意图判别: compose=%s for %r", result, text[:40])
        return result
    except Exception as e:
        logger.warning("[MasterAgent] LLM 意图判别失败, 回退关键词: %s", e)
        return None


# 内部链路 chunk 判据：菜包 agentic stream 会把工具中间结果（如 remove_model 返回的
# raw JSON）也当文本 chunk 吐出。这些含内部 actor 名（__room_/__shell_/__terrain_/
# __interior_）或工具状态键（remaining_actors/removed_actor/imported），不能给用户看。
# 注意：session_id/error_code/status_info 是 CAI 成功响应信封的固定字段，不是内部工具特征。
_INTERNAL_CHUNK_MARKERS = (
    "__room_", "__shell_", "__terrain_", "__interior_",
    "remaining_actors", "removed_actor", "imported_actor",
    '"status":', '"actor":', '"position":', '"rotation":', '"scale":',
    "scene_json_updated",
)


def _is_internal_tool_chunk(chunk) -> bool:
    """判断一个 stream chunk 是否是内部工具结果（不该暴露给用户）。"""
    if not isinstance(chunk, str):
        return False
    return any(m in chunk for m in _INTERNAL_CHUNK_MARKERS)


def _looks_like_tool_json(obj: Any) -> bool:
    if not isinstance(obj, dict):
        return False
    keys = set(obj.keys())
    return bool(
        {"actor", "position"} <= keys
        or {"actor", "scale"} <= keys
        or {"removed_actor", "remaining_actors"} & keys
        or {"imported_actor", "scene_json_updated"} & keys
    )


def _strip_visible_tool_json(text: str) -> str:
    """Remove raw tool JSON objects from otherwise natural-language text."""
    if not text:
        return ""

    out: List[str] = []
    i = 0
    n = len(text)
    while i < n:
        if text[i] != "{":
            out.append(text[i])
            i += 1
            continue
        depth = 0
        in_str = False
        escaped = False
        end = -1
        for j in range(i, n):
            ch = text[j]
            if in_str:
                if escaped:
                    escaped = False
                elif ch == "\\":
                    escaped = True
                elif ch == '"':
                    in_str = False
                continue
            if ch == '"':
                in_str = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end = j + 1
                    break
        if end == -1:
            out.append(text[i])
            i += 1
            continue
        candidate = text[i:end]
        try:
            data = json.loads(candidate)
        except Exception:  # noqa: BLE001
            out.append(text[i])
            i += 1
            continue
        if _looks_like_tool_json(data):
            i = end
            continue
        out.append(candidate)
        i = end

    cleaned = "".join(out)
    cleaned = re.sub(r"\n{3,}", "\n\n", cleaned)
    return cleaned.strip()


def _extract_cai_text_chunk(chunk) -> str:
    """从 CAI JSON 信封 chunk 中提取用户可见文本；非 JSON 则按纯文本处理。

    运行时可能返回 build_success_response 的 JSON 字符串，也可能由上层
    stream adapter 先转成 dict，或包一层 data/event。这里做宽松解析，但只
    抽取明确的自然语言字段，避免把 session_id/status_info 再暴露给用户。
    """
    if isinstance(chunk, dict):
        data = chunk
    elif isinstance(chunk, str):
        try:
            data = json.loads(chunk)
        except (json.JSONDecodeError, TypeError):
            return "" if _is_internal_tool_chunk(chunk) else chunk
    else:
        return ""

    def _collect_text(obj) -> List[str]:
        if isinstance(obj, str):
            if _is_internal_tool_chunk(obj):
                stripped = _strip_visible_tool_json(obj)
                return [stripped] if stripped else []
            return [_strip_visible_tool_json(obj)]
        if isinstance(obj, list):
            out: List[str] = []
            for item in obj:
                out.extend(_collect_text(item))
            return out
        if not isinstance(obj, dict):
            return []

        out: List[str] = []
        for content in obj.get("llm_content", []) or []:
            if not isinstance(content, dict):
                continue
            for part in content.get("part", []) or []:
                if not isinstance(part, dict):
                    continue
                if part.get("content_type") == "text":
                    text = str(part.get("content_text", "") or "")
                    if text:
                        stripped = _strip_visible_tool_json(text)
                        if stripped and not _is_internal_tool_chunk(stripped):
                            out.append(stripped)

        # 兼容 stream/event 包装层：只认常见自然语言字段，不递归扫所有键，
        # 避免 session_id/error_code/status_info 被误拼进回复。
        for key in ("content_text", "text", "content", "delta"):
            val = obj.get(key)
            if isinstance(val, str) and val:
                stripped = _strip_visible_tool_json(val)
                if stripped and not _is_internal_tool_chunk(stripped):
                    out.append(stripped)
        for key in ("data", "message", "payload", "response"):
            val = obj.get(key)
            if isinstance(val, (dict, list)):
                out.extend(_collect_text(val))
        return out

    if not isinstance(data, dict):
        return ""
    return "".join(_collect_text(data))


def is_compose_intent(text: str) -> bool:
    """关键词未命中后，判断是否仍是【整场景生成】意图（开放式）。

    设计：本函数只在 is_scene_command 关键词【未命中】时调用，所以这里：
    1. 明显闲聊/提问（_NON_SCENE_PATTERNS）→ False，不调 LLM（省延迟）
    2. 否则交 LLM 开放式判断（"做个海底世界""来个蒙古包草原"等清单外描述在此被捕获）
    3. LLM 不可用 → 保守 False（退回旧关键词行为，宁可漏判不误触发）
    """
    t = (text or "").strip()
    if not t or len(t) < 2:
        return False
    for pat in _NON_SCENE_PATTERNS:
        if re.match(pat, t):
            return False
    return bool(_llm_is_compose_intent(t))  # None(失败) → False


# ═══════════════════════════════════════════════════════════════════════════
# 意图三分类（compose / edit / chat）—— 取代关键词路由
# ═══════════════════════════════════════════════════════════════════════════

_INTENT_CLASSIFY_SYSTEM_PROMPT = """你是 LANChat 场景助手的意图分类器。把用户消息分成三类之一。

compose（从无到有生成一个完整的 3D 场景/环境）：
- "生成一个现代客厅" / "做一个赛博朋克酒吧街" / "来个蒙古包草原"
- "搭一个海底世界" / "布置一个北欧风卧室" / "整一个篝火露营场景"

edit（修改场景里【已有的】具体物体——有明确的操作对象 + 动作）：
- 缩放: "放大蒙古包3倍" / "把矮桌变小" / "蒙古包调大一点"
- 移动: "沙发往左移" / "把椅子挪到墙角"
- 旋转/删除/增加单个: "旋转椅子90度" / "删掉那盏灯" / "加一个茶几"

chat（闲聊/提问/评价/反馈——没有明确的"对某物做某操作"指令）：
- "你好" / "谢谢" / "客厅一般放什么" / "这个配色好看吗"
- "蒙古包没有调整好" / "感觉有点小" / "不太对"（只是评价/抱怨，没说具体怎么改）

判别要点：
- 明确"对哪个物体做什么操作（放大/缩小/移动/旋转/删除/加）" → edit
- 只是抱怨/评价/没说具体怎么改 → chat（即使提到物体名）
- 从零搭整个场景/环境 → compose

只输出 JSON：{"intent":"compose"} 或 {"intent":"edit"} 或 {"intent":"chat"}"""


_SCENE_WRITE_INTENT_SYSTEM_PROMPT = """你是 LANChat 3D 场景执行路由器。判断用户这条消息是否明确要求系统写入或修改持久化 3D 场景。

你必须先判断 scene_write_intent：
- true: 用户要求创建、布置、导入、删除、移动、缩放、旋转、替换或调整 3D 场景/物体。
- false: 用户只是在闲聊、提问、评价、让某个角色说话/表演/想象、或没有明确要求改变场景。

target 只能是：
- scene_world: 新建或重搭完整场景/环境/空间。
- existing_object: 修改现有场景中的具体物体或局部元素。
- agent_self: 请求被 @ 的角色本人回答、扮演、表演、想象或表达。
- conversation: 普通对话、问候、解释、建议、总结。
- abstract_topic: 讨论想法/概念，但没有要求写入场景。

intent 只能是：
- compose: 从无到有创建完整场景/空间。
- edit: 修改已有场景对象或局部。
- chat: 不应执行场景写入。

重要原则：
- 消息里出现场景名、地点、动作或想象画面，不等于要写入 3D 场景。
- 对角色说“我想看你……”“你是谁”“你觉得……”通常是 agent_self 或 conversation，scene_write_intent=false。
- 只有用户明确让系统“生成/做/搭建/布置/设计/加入/删除/移动/调整”场景或物体时才 scene_write_intent=true。
- 不确定时选 chat，scene_write_intent=false，confidence 低于 0.55。

输出严格 JSON：
{"intent":"compose|edit|chat","scene_write_intent":true|false,"target":"scene_world|existing_object|agent_self|conversation|abstract_topic","confidence":0到1,"reason":"一句简短理由"}"""


def _coerce_scene_intent_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        low = value.strip().lower()
        if low in {"true", "1", "yes", "y", "是", "yes."}:
            return True
        if low in {"false", "0", "no", "n", "否", ""}:
            return False
    return bool(value)


def _coerce_scene_intent_confidence(value: Any) -> float:
    try:
        confidence = float(value)
    except Exception:  # noqa: BLE001
        return 0.0
    return max(0.0, min(1.0, confidence))


def _extract_json_object(raw: str) -> str:
    raw = (raw or "").strip()
    if "```" in raw:
        s = raw.find("{")
        e = raw.rfind("}")
        if s != -1 and e != -1:
            return raw[s:e + 1]
    return raw


def _llm_classify_scene_intent(text: str, timeout: float = 20.0) -> Optional[Dict[str, Any]]:
    """LLM 结构化判断：区分普通角色对话和真正的场景写入请求。"""
    if not text or not text.strip():
        return None
    try:
        from concurrent.futures import ThreadPoolExecutor
        from langchain_core.messages import HumanMessage, SystemMessage
        from Quasar.ai_models.base_pool.registry import get_chat_model

        def _do():
            model = get_chat_model(temperature=0, request_timeout=18.0)
            return model.invoke([
                SystemMessage(content=_SCENE_WRITE_INTENT_SYSTEM_PROMPT),
                HumanMessage(content=text.strip()[:500]),
            ])

        ex = ThreadPoolExecutor(max_workers=1)
        try:
            resp = ex.submit(_do).result(timeout=timeout)
        finally:
            ex.shutdown(wait=False)

        raw = _extract_json_object(resp.content if hasattr(resp, "content") else str(resp))
        data = json.loads(raw)
        if not isinstance(data, dict):
            return None
        intent = str(data.get("intent") or "chat").strip().lower()
        if intent not in {"compose", "edit", "chat"}:
            intent = "chat"
        target = str(data.get("target") or "").strip().lower()
        if target not in {"scene_world", "existing_object", "agent_self", "conversation", "abstract_topic"}:
            target = ""
        decision = {
            "intent": intent,
            "scene_write_intent": _coerce_scene_intent_bool(data.get("scene_write_intent")),
            "target": target,
            "confidence": _coerce_scene_intent_confidence(data.get("confidence", 0.0)),
            "reason": str(data.get("reason") or "")[:300],
        }
        logger.info(
            "[MasterAgent] LLM 场景写入意图: intent=%s write=%s target=%s confidence=%.2f for %r",
            decision["intent"],
            decision["scene_write_intent"],
            decision["target"],
            decision["confidence"],
            text[:40],
        )
        return decision
    except Exception as e:
        logger.warning("[MasterAgent] LLM 场景写入意图判别失败, 回退旧分类: %s", e)
        return None


def _intent_from_scene_intent_decision(decision: Any) -> Optional[str]:
    if not isinstance(decision, dict):
        return None
    intent = str(decision.get("intent") or "chat").strip().lower()
    if intent not in {"compose", "edit", "chat"}:
        return None
    scene_write_intent = _coerce_scene_intent_bool(decision.get("scene_write_intent"))
    confidence = _coerce_scene_intent_confidence(decision.get("confidence", 0.0))
    target = str(decision.get("target") or "").strip().lower()
    if not scene_write_intent or confidence < 0.55:
        return "chat"
    if intent == "compose":
        return "compose" if target in {"scene_world", ""} else "chat"
    if intent == "edit":
        return "edit" if target in {"existing_object", "scene_world", ""} else "chat"
    return "chat"


def _llm_classify_intent(text: str, timeout: float = 20.0) -> Optional[str]:
    """LLM 三分类：返回 "compose" | "edit" | "chat"；失败返回 None（交调用方兜底）。

    超时同 _llm_is_compose_intent：future(20s) > HTTP request_timeout(18s)。
    """
    if not text or not text.strip():
        return None
    try:
        from concurrent.futures import ThreadPoolExecutor
        from langchain_core.messages import HumanMessage, SystemMessage
        from Quasar.ai_models.base_pool.registry import get_chat_model

        def _do():
            model = get_chat_model(temperature=0, request_timeout=18.0)
            return model.invoke([
                SystemMessage(content=_INTENT_CLASSIFY_SYSTEM_PROMPT),
                HumanMessage(content=text.strip()[:500]),
            ])

        ex = ThreadPoolExecutor(max_workers=1)
        try:
            resp = ex.submit(_do).result(timeout=timeout)
        finally:
            ex.shutdown(wait=False)

        raw = _extract_json_object(resp.content if hasattr(resp, "content") else str(resp))
        data = json.loads(raw)
        intent = str(data.get("intent", "")).strip().lower()
        if intent in ("compose", "edit", "chat"):
            logger.info("[MasterAgent] LLM 意图分类: %s for %r", intent, text[:40])
            return intent
        return None
    except Exception as e:
        logger.warning("[MasterAgent] LLM 意图分类失败, 回退关键词: %s", e)
        return None


def classify_intent(text: str) -> str:
    """意图三分类路由：compose（生成整场景）/ edit（改已有物体）/ chat（闲聊）。

    优先 LLM 语义分类（关键词漏判/误判的根治）；明显闲聊走快速路径省延迟；
    LLM 不可用时退回关键词兜底（is_scene_command 命中再用 is_compose_request 分 compose/edit）。
    """
    t = (text or "").strip()
    if not t or len(t) < 2:
        return "chat"
    # 快速路径：明显问候/提问/致谢 → chat（零延迟，不调 LLM）
    for pat in _NON_SCENE_PATTERNS:
        if re.match(pat, t):
            return "chat"
    # LLM 结构化路由（权威）：普通角色对话即使提到想象场景，也不能写入 3D 场景。
    service = get_intent_understanding_service()
    decision = service.classify(t, allow_llm=False)
    if decision.intent in ("generation_start", "plan_drafting", "plan_revision"):
        return "compose"
    if decision.intent in (
        "intervention_add",
        "intervention_modify",
        "intervention_delete",
        "post_generation_add",
        "final_adjustment_request",
    ):
        return "edit"
    if decision.reason != "fallback default":
        return "chat"
    legacy_decision = _llm_classify_scene_intent(t)
    r = _intent_from_scene_intent_decision(legacy_decision)
    if r in ("compose", "edit", "chat"):
        return r
    r = _llm_classify_intent(t)
    if r in ("compose", "edit", "chat"):
        return r
    # LLM 失败兜底：退回关键词。命中场景关键词再分 compose/edit，否则 chat
    if is_scene_command(t):
        from .scene_composer import is_compose_request
        return "compose" if (is_compose_request(t)) else "edit"
    return "chat"


# ═══════════════════════════════════════════════════════════════════════════
# Specialist 注册表
# ═══════════════════════════════════════════════════════════════════════════

class Specialist:
    """一个 specialist 模块 — 独立的 name + style_bible + 能力描述。"""

    def __init__(self, key: str, name: str, style_bible: Dict[str, Any],
                 capabilities: List[str], keywords: List[str]) -> None:
        self.key = key
        self.name = name
        self.style_bible = style_bible
        self.capabilities = capabilities
        self.keywords = keywords

    def match_persona(self, persona: str) -> bool:
        if not persona:
            return False
        persona_lower = persona.lower()
        return any(kw in persona_lower for kw in self.keywords)

    def inject_prompt(self, base_prompt: str) -> str:
        bible_text = self._format_bible()
        caps = "\n".join(f"  - {c}" for c in self.capabilities)
        return f"""{base_prompt}

## 当前专家: {self.name}

风格约束:
{bible_text}

擅长:
{caps}
"""

    def _format_bible(self) -> str:
        sb = self.style_bible
        parts = []
        if sb.get("theme"):       parts.append(f"主题: {sb['theme']}")
        if sb.get("color_palette"): parts.append(f"色调: {', '.join(sb['color_palette'])}")
        if sb.get("materials"):   parts.append(f"材质: {', '.join(sb['materials'])}")
        if sb.get("mood"):        parts.append(f"氛围: {sb['mood']}")
        if sb.get("avoid"):       parts.append(f"避讳: {', '.join(sb['avoid'])}")
        if sb.get("lighting"):    parts.append(f"光照: {sb['lighting']}")
        return "\n".join(parts) if parts else "无特定风格约束"


SPECIALISTS: Dict[str, Specialist] = {
    "cyberpunk": Specialist(
        key="cyberpunk", name="赛博朋克设计师",
        style_bible={
            "theme": "cyberpunk wasteland",
            "color_palette": ["#1a1a2e", "#16213e", "#0f3460", "#e94560"],
            "materials": ["weathered metal", "neon glass", "concrete"],
            "lighting": "low ambient + colored neon",
            "mood": "dystopian, gritty, vibrant",
            "avoid": ["pastoral", "bright", "medieval", "田园", "明亮暖色"],
        },
        capabilities=["暗色调金属家具", "霓虹灯光布置", "工业废墟风格", "赛博朋克酒吧/街道"],
        keywords=["赛博朋克", "cyberpunk", "暗黑", "霓虹"],
    ),
    "minimalist": Specialist(
        key="minimalist", name="北欧极简设计师",
        style_bible={
            "theme": "scandinavian minimalist",
            "color_palette": ["#f5f0e8", "#e8e0d5", "#d4c9b8", "#ffffff"],
            "materials": ["light oak", "linen", "wool", "matte ceramic"],
            "lighting": "soft natural + warm ambient",
            "mood": "calm, airy, clean",
            "avoid": ["ornate", "gold", "baroque", "繁复", "镀金", "深色"],
        },
        capabilities=["浅木色家具布置", "明亮通透空间", "极简线条", "北欧客厅/卧室"],
        keywords=["极简", "北欧", "minimalist", "scandinavian", "简约", "干净", "明亮"],
    ),
    "chinese": Specialist(
        key="chinese", name="新中式设计师",
        style_bible={
            "theme": "modern chinese",
            "color_palette": ["#4a3728", "#8b7355", "#f5f0e8", "#2c2c2c"],
            "materials": ["walnut wood", "bamboo weave", "rice paper", "dark elm"],
            "lighting": "warm indirect + paper lantern",
            "mood": "elegant, serene, scholarly",
            "avoid": ["neon", "plastic", "chrome", "霓虹", "塑料"],
        },
        capabilities=["胡桃木/黑檀木家具", "茶室/书房布局", "水墨配色", "新中式客厅"],
        keywords=["中式", "新中式", "茶室", "书房", "胡桃木", "禅", "雅致"],
    ),
    "industrial": Specialist(
        key="industrial", name="工业风设计师",
        style_bible={
            "theme": "industrial loft",
            "color_palette": ["#3a3a3a", "#6b6b6b", "#8b4513", "#d4d4d4"],
            "materials": ["concrete", "black steel", "exposed brick", "distressed leather"],
            "lighting": "exposed bulb + metal cage",
            "mood": "raw, urban, masculine",
            "avoid": ["floral", "lace", "pastel", "花卉", "蕾丝", "粉嫩"],
        },
        capabilities=["混凝土/黑铁材质", "loft 公寓/酒吧", "裸露结构", "工业风空间"],
        keywords=["工业风", "industrial", "loft", "仓库", "铁艺", "粗犷"],
    ),
    "lighting": Specialist(
        key="lighting", name="灯光师",
        style_bible={},
        capabilities=["壁灯/落地灯/吊灯布置", "霓虹/灯带氛围", "色温选择", "光影层次"],
        keywords=["灯光", "灯", "光照", "照明", "光源", "氛围光", "霓虹灯", "色温", "亮", "暗"],
    ),
    "plant": Specialist(
        key="plant", name="绿植顾问",
        style_bible={},
        capabilities=["室内盆栽布置", "植物与空间匹配", "绿植组合", "花卉摆放"],
        keywords=["绿植", "植物", "盆栽", "花", "绿化", "花园", "树木"],
    ),
    "material": Specialist(
        key="material", name="材质顾问",
        style_bible={},
        capabilities=["金属/木材/玻璃/布料/石材搭配", "配色方案", "材质冲突检测"],
        keywords=["材质", "配色", "颜色", "色调", "材料", "质感", "搭配"],
    ),
    "tidying": Specialist(
        key="tidying", name="整理助手",
        style_bible={},
        capabilities=["发现不协调布局", "重叠/悬空检测", "整理建议", "快速优化"],
        keywords=["整理", "检查", "检查一下", "看看", "有什么问题", "优化", "调整布局", "重新布置"],
    ),
}

_GENERALIST = Specialist(
    key="generalist", name="场景设计大师",
    style_bible={},
    capabilities=[
        "全风格场景设计（赛博朋克/北欧/中式/工业风）",
        "家具布局与空间规划",
        "灯光设计与氛围营造",
        "绿植与装饰摆放",
        "材质与配色建议",
        "布局检查与优化",
    ],
    keywords=[],
)

_GENERALIST_INTRO = """我是 **场景设计大师**，具备以下全部能力：

🎨 风格 — 赛博朋克 / 北欧极简 / 新中式 / 工业风
💡 灯光 — 壁灯、落地灯、霓虹灯带、色温
🌿 绿植 — 盆栽、花卉、空间绿化
🔧 材质 — 金属、木材、玻璃、布料搭配
📐 整理 — 检查重叠、悬空、比例异常

**场景指令** (直接对我说):
  • 加个台灯 | 把沙发往右移 | 删掉茶几
  • 把酒吧布置得更有氛围感
  • 检查一下布局有什么问题

**内置命令**:
  /help — 显示此帮助
  /总结 — 根据群聊讨论生成风格方案
  /检查 — 对当前场景做风格巡检"""


class PersonaRouter:
    """根据 persona 匹配 specialist。"""

    def __init__(self):
        self._specialists = dict(SPECIALISTS)

    def route(self, persona: str) -> Specialist:
        if not persona or persona.strip() == "你是一个有帮助的助手。":
            return _GENERALIST
        for key in ["cyberpunk", "minimalist", "chinese", "industrial",
                     "lighting", "plant", "material", "tidying"]:
            spec = self._specialists[key]
            if spec.match_persona(persona):
                logger.info("[MasterAgent] persona → %s (%s)", spec.name, spec.key)
                return spec
        return _GENERALIST

    def register(self, specialist: Specialist) -> None:
        self._specialists[specialist.key] = specialist

    def list_all(self) -> List[Dict[str, Any]]:
        return [{"key": k, "name": s.name, "capabilities": s.capabilities, "keywords": s.keywords}
                for k, s in self._specialists.items()]


# ═══════════════════════════════════════════════════════════════════════════
# SummaryAgent — 替换 SummaryService 的 ai_chat 回调
# ═══════════════════════════════════════════════════════════════════════════

class SummaryAgent:
    """摘要回调 — 增强版群聊压缩，融入场景上下文。

    签名: (prompt: str) -> str  (匹配 SummaryService 的 ai_chat 接口)
    """

    def __init__(self, fallback_chat: Callable[[str], str] = None) -> None:
        self._fallback_chat = fallback_chat

    def __call__(self, prompt: str) -> str:
        """生成群聊摘要，融入场景感知。

        LANChat 的 SummaryService 传入的 prompt 格式:
          "你是对话摘要助手。把以下群聊压缩成简洁要点..."
          + [已有摘要] + [新增对话] + [合并后的摘要]
        """
        try:
            # 尝试用 LLM 生成增强摘要 (带场景上下文)
            return self._enhanced_summary(prompt)
        except Exception as e:
            logger.warning("[SummaryAgent] enhanced summary failed: %s, falling back", e)
            return self._fallback_summary(prompt)

    def _enhanced_summary(self, prompt: str) -> str:
        """生成增强摘要 — 提取场景相关的风格共识。"""
        # 注入场景感知指令
        enhanced_prompt = prompt.replace(
            "把以下群聊压缩成简洁要点",
            "把以下群聊压缩成简洁要点。特别关注: 风格偏好、色调、材质、灯光、场景类型等场景设计相关的共识。如有明确的风格讨论，在摘要末尾用 [风格共识] 标注。"
        )
        return self._call_llm(enhanced_prompt)

    def _fallback_summary(self, prompt: str) -> str:
        if self._fallback_chat:
            return self._fallback_chat(prompt)
        return self._call_llm(prompt)

    def _call_llm(self, prompt: str) -> str:
        try:
            from plugins.AITool.main import AITool
            from Quasar.cai.protocol.request import ChatRequest
            req = ChatRequest.from_text(text=prompt, metadata={"skip_conversation_store": True})
            chunks = AITool._cai_app.chat(req)
            return "".join(chunks).strip()
        except Exception as e:
            raise RuntimeError(f"LLM 调用失败: {e}")


# ═══════════════════════════════════════════════════════════════════════════
# MasterAgent — 统一入口 (AgentRunner 回调)
# ═══════════════════════════════════════════════════════════════════════════

class MasterAgent:
    """Master Agent — 接管 LANChat AgentRunner 的全部能力。

    能力:
      - 场景编辑 → AgentCoordinator
      - 普通聊天 → CAIApp + specialist 上下文
      - 讨论总结 → GroupAgent
      - 风格巡检 → GroupAgent
      - 能力查询 → /help
    """

    def __init__(
        self,
        fallback_chat: Callable[[str, List[str]], str] = None,
        global_style: Dict[str, Any] = None,
        scene_max_items: int = 8,  # 阶段测试期降到 4，省 hunyuan 消耗（之前 12）
    ) -> None:
        self._fallback_chat = fallback_chat
        self._global_style = global_style or {}
        self._scene_max_items = max(1, int(scene_max_items))  # 单次场景生成物体数上限
        self._router = PersonaRouter()
        self._coordinator = None
        self._group_agent = None
        self._planner = None
        self._collaboration = None

    @property
    def coordinator(self):
        if self._coordinator is None:
            from .coordinator import AgentCoordinator
            self._coordinator = AgentCoordinator()
        return self._coordinator

    @property
    def group_agent(self):
        if self._group_agent is None:
            from .group_agent import GroupAgent
            self._group_agent = GroupAgent(ai_chat=self._group_ai_chat)
        return self._group_agent

    @property
    def planner(self):
        if self._planner is None:
            from .multi_step_planner import MultiStepPlanner
            self._planner = MultiStepPlanner(coordinator=self.coordinator)
        return self._planner

    @property
    def collaboration(self):
        if self._collaboration is None:
            from .collaboration import get_collaboration_manager
            self._collaboration = get_collaboration_manager()
        return self._collaboration

    def _group_ai_chat(self, system: str, messages: list) -> str:
        """GroupAgent 的 ai_chat 回调 — 调用 CAIApp。"""
        try:
            from plugins.AITool.main import AITool
            from Quasar.cai.protocol.request import ChatRequest
            convo = "\n".join(messages) if isinstance(messages, list) else str(messages)
            text = f"{system}\n\n{convo}"
            req = ChatRequest.from_text(text=text, metadata={"skip_conversation_store": True})
            chunks = AITool._cai_app.chat(req)
            return "".join(_extract_cai_text_chunk(c) for c in chunks).strip() or "{}"
        except Exception as e:
            logger.warning("[MasterAgent] group_ai_chat failed: %s, using fallback", e)
            if self._fallback_chat:
                try:
                    return self._fallback_chat(system, messages if isinstance(messages, list) else [str(messages)])
                except Exception:
                    pass
            return "{}"

    # ── 对外接口 ───────────────────────────────────────────────────

    def __call__(self, system: str, messages: List[str]) -> str:
        """LANChat AgentRunner 回调入口。

        关键: 必须始终返回字符串, 绝不抛异常 —— 否则 LANChat 线程池
        会吞掉异常, _on_done 永不执行, 群聊收不到任何回复。
        """
        try:
            trigger = self._extract_trigger_text(messages)
            logger.info("[MasterAgent] __call__ trigger=%r persona=%r", trigger[:80], (system or "")[:40])

            slash_command = trigger.strip().split(maxsplit=1)[0] if trigger.strip().startswith("/") else ""
            if slash_command and is_deprecated_user_workflow_command(slash_command):
                logger.info("[MasterAgent] deprecated workflow command blocked: %s", slash_command)
                return DEPRECATED_WORKFLOW_COMMAND_MESSAGE

            # 0. 显式文件路径导入 → 跳过 LLM，直接调引擎 import_model
            file_path = self._extract_file_path(trigger)
            if file_path:
                logger.info("[MasterAgent] routing → direct import %s", file_path)
                return self._handle_direct_import(file_path)

            # 1. 内置命令
            cmd = is_builtin_command(trigger)
            if cmd == "help":
                return self._handle_help(system)
            if cmd == "summary":
                return self._handle_summary(system, messages)
            if cmd == "patrol":
                return self._handle_patrol(system)

            # 1.5 生成前轻量确认：只拦计划型请求；明确“确认/直接生成”会继续进入 compose。
            agent_name = self._extract_current_agent_name(messages) or self._router.route(system).name
            gate_action, gate_payload = get_lanchat_scene_runtime().handle_planning_gate(agent_name, trigger)
            if gate_action == "reply":
                logger.info("[MasterAgent] routing → planning confirmation gate")
                return str(gate_payload or "")
            if gate_action == "compose":
                logger.info("[MasterAgent] routing → compose (confirmed plan)")
                return self._handle_scene(str(gate_payload or trigger), system, messages, force_compose=True)

            # 2. 意图三分类（compose / edit / chat）——语义分类取代关键词路由。
            #    关键词路由两类都翻车：真编辑漏判掉聊天（"把矮桌变小""蒙古包调大"），
            #    抱怨误判进编辑（"蒙古包没有调整好"命中"调整"）。改用 LLM 语义分类。
            intent_class = classify_intent(trigger)
            if intent_class == "compose":
                # 整场景生成（含清单外描述"海底世界""蒙古包草原"）→ force_compose 跳内层关键词门
                logger.info("[MasterAgent] routing → compose (意图分类)")
                return self._handle_scene(trigger, system, messages, force_compose=True)
            if intent_class == "edit":
                # 改已有物体（放大/移动/旋转/删除）→ 专用 edit 路径：
                # 读引擎 actor 当前 transform + LLM 解析相对量（"3倍""y轴+2"）→ 算绝对目标 → set_actor_transform。
                # 不走 _handle_scene/coordinator（那条为"add 新物体算落点"设计，move/scale 会崩+丢失相对量）。
                logger.info("[MasterAgent] routing → edit (意图分类，专用变换执行)")
                return self._handle_edit(trigger, messages)

            # 3. 普通聊天（含评价/抱怨/提问，无明确操作指令）
            logger.info("[MasterAgent] routing → chat (意图分类)")
            return self._handle_chat(system, messages)
        except Exception as e:
            logger.exception("[MasterAgent] __call__ 异常, 返回兜底回复")
            msg = str(e).lower()
            if any(k in msg for k in ("ssl", "eof", "timeout", "connection",
                                      "reset", "refused", "unreachable")):
                return "🌐 网络好像不太稳定，请稍后再发一次～"
            return "🤖 抱歉，刚才处理消息时出了点问题，请换个说法再试一次。"

    # ── 直接文件导入 ──────────────────────────────────────────────

    def _handle_direct_import(self, file_path: str) -> str:
        """直接导入指定模型文件到引擎场景（跳过 LLM 和混元生成）。"""
        import os as _os
        try:
            from plugins.AITool.cai_extensions.flows.scene_composition_workflow.helpers import get_tool
            tool = get_tool("import_model")
            if tool is None:
                return "⚠️ import_model 工具不可用，请确认引擎已启动且场景已加载。"
            actor_name = _os.path.splitext(_os.path.basename(file_path))[0]
            tool.invoke({
                "model_path": file_path,
                "actor_name": actor_name,
                "position": [0.0, 0.0, 0.0],
            })
            logger.info("[MasterAgent] direct import success: %s", file_path)
            return f"✅ 已导入「{actor_name}」到场景原点 (0,0,0)。"
        except Exception as e:
            logger.exception("[MasterAgent] direct import failed: %s", e)
            return f"⚠️ 导入失败：{e}"

    # ── 内置命令 ──────────────────────────────────────────────────

    def _handle_help(self, persona: str) -> str:
        specialist = self._router.route(persona)
        if specialist.key == "generalist":
            return _GENERALIST_INTRO
        caps = "\n".join(f"  • {c}" for c in specialist.capabilities)
        bible = specialist._format_bible()
        style_section = f"\n\n当前风格:\n{bible}" if bible else ""
        return f"**{specialist.name}** 擅长:\n{caps}{style_section}"

    def _handle_summary(self, persona: str, messages: List[str]) -> str:
        """讨论总结 — 用 GroupAgent 生成 Style Bible。"""
        # 从 messages 中提取聊天记录喂给 GroupAgent
        for msg in messages:
            if msg.startswith("[此前对话摘要]") or msg.startswith("[摘要]"):
                continue
            if ": " in msg:
                user, text = msg.split(": ", 1)
                self.group_agent.on_chat_message(user, text)

        if not self.group_agent.should_summarize():
            return "📝 讨论内容还不够丰富，继续聊聊风格偏好、色调、功能需求吧。至少需要 2 人参与讨论。"

        result = self.group_agent.try_summarize()
        if not result:
            return "⚠️ 总结生成失败，请稍后重试。"

        # 格式化输出
        plan = result.get("proposed_plan", {})
        bible = plan.get("style_bible", {})
        consensus = result.get("consensus_analysis", {})

        lines = ["📋 **讨论总结**\n"]
        lines.append(f"共识度: theme={consensus.get('theme_agreement', 0):.0%}  type={consensus.get('type_agreement', 0):.0%}")
        if bible:
            lines.append(f"\n**Style Bible**:")
            if bible.get("theme"):     lines.append(f"  主题: {bible['theme']}")
            if bible.get("mood"):      lines.append(f"  氛围: {bible['mood']}")
            if bible.get("materials"): lines.append(f"  材质: {', '.join(bible['materials'])}")
            if bible.get("lighting"):  lines.append(f"  光照: {bible['lighting']}")
            if bible.get("avoid"):     lines.append(f"  避讳: {', '.join(bible['avoid'])}")
        if plan.get("scene_name"):
            lines.append(f"\n场景名: {plan['scene_name']}")
        uncertainties = result.get("uncertainties", [])
        if uncertainties:
            lines.append(f"\n⚠️ 待确认: {', '.join(uncertainties)}")
        lines.append("\n确认后即可开始布置！直接对我说场景指令即可。")

        return "\n".join(lines)

    def _handle_patrol(self, persona: str) -> str:
        """风格巡检 — 用 GroupAgent 检查风格一致性。"""
        specialist = self._router.route(persona)
        bible = specialist.style_bible if specialist.key != "generalist" else self._global_style

        if not bible:
            return "📝 还没有设置风格标准。先讨论一下想要的风格，然后输入 /总结 来生成 Style Bible。"

        # 同步巡检 (不依赖 VLM, 用 GroupAgent 的规则检查)
        self.group_agent.on_operation_event({"data": {"action": "patrol_check"}, "metadata": {}})
        if not self.group_agent.should_patrol(user_requested=True):
            return f"✅ 操作量较少 ({self.group_agent._operation_count} 次)，尚未触发巡检阈值。当前偏离率: {self.group_agent.deviation_rate:.0%}"

        result = self.group_agent.try_patrol(style_bible=bible, user_requested=True)
        if not result:
            return "⚠️ 巡检执行失败，请稍后重试。"

        score = result.get("overall_consistency", "?")
        violations = result.get("violations", [])
        recs = result.get("recommendations", [])

        lines = [f"🔍 **风格巡检报告** (一致性: {score}/100)\n"]
        if violations:
            lines.append("**偏离项:**")
            for v in violations[:5]:
                lines.append(f"  ⚠️ {v.get('object_id', '?')}: {v.get('description', '')} (评分 {v.get('consistency_score', '?')})")
                for s in v.get("suggestions", []):
                    lines.append(f"    💡 {s}")
        else:
            lines.append("✅ 未检测到明显偏离")
        if recs:
            lines.append(f"\n**建议:** {', '.join(recs[:3])}")

        return "\n".join(lines)

    # ── 场景指令 ──────────────────────────────────────────────────

    _EDIT_EXEC_SYSTEM_PROMPT = """你是 3D 场景编辑执行助手。你拥有直接操作引擎场景的工具（移动/缩放/旋转/删除物体），\
必须【实际调用工具完成操作】，不要只回复文字。

当前场景里可编辑的物体（这些是引擎里的真实 actor 名字，调工具时必须用这些名字）：
{actors}

执行规则：
- 用户说的物体名可能是简称：用户说"蒙古包"，实际 actor 名是 "__shell_蒙古包"——调工具时用真实名字。
        - 相对量要基于物体【当前 transform】（上面列出了）计算出绝对目标值再调工具：
          * "放大3倍"=当前 scale 各分量 ×3；"缩小一半"=×0.5；"变大"≈×1.5；"变小"≈×0.7
          * "y轴向上移动2"=当前 pos 的 y +2；"往左1米"=x -1（坐标系 X+右/Y+上/Z+屏幕内）
          * 项目底层 rotation 使用【弧度】。用户说"旋转90度"时，先把 90 度转换为 1.5708 弧度，再做当前 rot 的 y +1.5708。
- 设置绝对变换用 set_actor_transform（传 actor 真实名 + position/rotation/scale）；删除用对应删除工具。
- 完成后用一句中文确认你做了什么（含物体名和新数值）。"""

    def _actor_display_name(self, name: str) -> str:
        display = str(name or "")
        for prefix in ("__shell_", "__asset_"):
            if display.startswith(prefix):
                return display[len(prefix):]
        return display

    @staticmethod
    def _canonical_edit_actor_name(name: str) -> str:
        try:
            from plugins.AITool.services.terrain_component_resolver import canonical_actor_id
        except Exception:  # noqa: BLE001
            try:
                from ...services.terrain_component_resolver import canonical_actor_id  # type: ignore
            except Exception:  # noqa: BLE001
                canonical_actor_id = None  # type: ignore
        if callable(canonical_actor_id):
            return str(canonical_actor_id(name) or name)
        return str(name or "")

    @staticmethod
    def _looks_like_boundary_reference(user_text: str) -> bool:
        text = str(user_text or "")
        return any(token in text for token in (
            "_terrain_boundary",
            "__terrain_boundary",
            "terrain_boundary",
            "地形边界",
            "场地边界",
            "边界",
            "栅栏",
            "围栏",
        ))

    @classmethod
    def _is_system_edit_actor(cls, name: str) -> bool:
        canonical = cls._canonical_edit_actor_name(name)
        return canonical in {"__terrain_boundary", "__room_terrain"} or canonical.startswith("__terrain_")

    def _pick_edit_actor(self, user_text: str, actors: List[Any]) -> Any | None:
        if self._looks_like_boundary_reference(user_text):
            for actor in actors:
                name = str(getattr(actor, "name", "") or "")
                if self._canonical_edit_actor_name(name) == "__terrain_boundary":
                    return actor
        matches: list[tuple[int, Any]] = []
        for actor in actors:
            name = str(getattr(actor, "name", "") or "")
            canonical = self._canonical_edit_actor_name(name)
            if canonical and canonical in user_text:
                matches.append((len(canonical) + 100, actor))
                continue
            display = self._actor_display_name(name)
            if name and name in user_text:
                matches.append((len(name), actor))
                continue
            if display and display in user_text:
                matches.append((len(display), actor))
        if not matches:
            return None
        matches.sort(key=lambda item: item[0], reverse=True)
        return matches[0][1]

    def _parse_fast_scale_factor(self, user_text: str) -> float | None:
        text = user_text.strip()
        numeric = re.search(r"([0-9]+(?:\.[0-9]+)?)\s*倍", text)
        if numeric and any(k in text for k in ("放大", "变大", "扩大")):
            return max(0.05, float(numeric.group(1)))
        if numeric and any(k in text for k in ("缩小", "变小")):
            return max(0.05, 1.0 / max(0.05, float(numeric.group(1))))
        if "一半" in text and any(k in text for k in ("缩小", "变小")):
            return 0.5
        if any(k in text for k in ("最大", "大一点", "变大", "放大")):
            return 1.35
        if any(k in text for k in ("小一点", "变小", "缩小")):
            return 0.75
        return None

    def _parse_fast_position_delta(self, user_text: str, actor: Any) -> List[float] | None:
        text = user_text.strip()
        try:
            pos = [float(v) for v in actor.get_position()]
        except Exception:
            pos = [0.0, 0.0, 0.0]
        step = 2.0 if any(k in text for k in ("远一点", "移远", "调远")) else 1.0
        if "放中间" in text or "居中" in text or "到中间" in text:
            return [-pos[0], 0.0, -pos[2]]
        if "靠左" in text or "往左" in text or "左墙" in text:
            return [-step, 0.0, 0.0]
        if "靠右" in text or "往右" in text or "右墙" in text:
            return [step, 0.0, 0.0]
        if "往前" in text or "靠前" in text:
            return [0.0, 0.0, step]
        if "往后" in text or "靠后" in text:
            return [0.0, 0.0, -step]
        if "移远" in text or "远一点" in text or "调远" in text:
            direction = 1.0 if pos[2] >= 0.0 else -1.0
            if abs(pos[2]) < 0.2:
                direction = -1.0
            return [0.0, 0.0, direction * step]
        if "近一点" in text or "靠近" in text:
            direction = -1.0 if pos[2] >= 0.0 else 1.0
            return [0.0, 0.0, direction * step]
        return None

    def _parse_fast_rotation_delta(self, user_text: str) -> float | None:
        text = user_text.strip()
        if not any(k in text for k in ("旋转", "转一下", "转动", "转到", "朝向", "方向")):
            return None
        numeric = re.search(r"([0-9]+(?:\.[0-9]+)?)\s*度", text)
        angle = float(numeric.group(1)) if numeric else 90.0
        if any(k in text for k in ("逆时针", "往左转", "左转")):
            angle = -angle
        return math.radians(angle)

    def _parse_fast_color(self, user_text: str) -> tuple[str, List[float]] | None:
        colors = {
            "红": ("红色", [1.0, 0.08, 0.06]),
            "蓝": ("蓝色", [0.08, 0.28, 1.0]),
            "绿": ("绿色", [0.08, 0.65, 0.18]),
            "黄": ("黄色", [1.0, 0.82, 0.08]),
            "白": ("白色", [0.92, 0.92, 0.88]),
            "黑": ("黑色", [0.03, 0.03, 0.03]),
            "灰": ("灰色", [0.45, 0.45, 0.45]),
            "金": ("金色", [1.0, 0.68, 0.16]),
            "银": ("银色", [0.75, 0.76, 0.78]),
            "粉": ("粉色", [1.0, 0.45, 0.72]),
            "紫": ("紫色", [0.55, 0.18, 0.85]),
            "棕": ("棕色", [0.42, 0.22, 0.10]),
        }
        if not any(k in user_text for k in ("改色", "换色", "变成", "颜色", "涂成")):
            return None
        for key, value in colors.items():
            if key in user_text:
                return value
        return None

    def _try_apply_actor_color(self, actor: Any, rgb: List[float]) -> bool:
        candidates = [
            getattr(actor, "set_color", None),
            getattr(actor, "set_diffuse", None),
        ]
        optics = getattr(actor, "_optics", None)
        if optics is not None:
            candidates.extend([
                getattr(optics, "set_color", None),
                getattr(optics, "set_diffuse", None),
                getattr(optics, "set_base_color", None),
            ])
        for setter in candidates:
            if not callable(setter):
                continue
            try:
                setter(rgb)
                return True
            except TypeError:
                try:
                    setter(float(rgb[0]), float(rgb[1]), float(rgb[2]))
                    return True
                except Exception:
                    continue
            except Exception:
                continue
        return False

    def _looks_like_fast_edit_request(self, user_text: str) -> bool:
        return any(k in user_text for k in (
            "放大", "缩小", "变大", "变小", "贴地", "穿模", "底座",
            "移远", "靠墙", "靠左", "靠右", "往前", "往后", "居中",
            "删除", "删掉", "移除", "不要这个", "旋转", "转一下",
            "改色", "换色", "颜色", "涂成",
            "换成", "低矮", "藤蔓", "木栏", "围栏", "栅栏", "边界",
        ))

    def _candidate_actor_reply(self, actors: List[Any]) -> str:
        names = [self._actor_display_name(str(getattr(a, "name", "") or "")) for a in actors[:8]]
        names = [n for n in names if n]
        if not names:
            return "我没找到可编辑的物体。"
        return "我没找到你说的那个物体。你可以点名这些物体之一：" + "、".join(names)

    def _try_fast_transform_edit(self, user_text: str, actors: List[Any]) -> str | None:
        actor = self._pick_edit_actor(user_text, actors)
        if actor is None:
            return None

        name = str(getattr(actor, "name", "") or "")
        display = self._actor_display_name(name)
        changed_parts: list[str] = []
        scale_factor = self._parse_fast_scale_factor(user_text)
        try:
            if any(k in user_text for k in ("删除", "删掉", "移除", "不要这个")):
                setter = getattr(actor, "set_visible", None)
                if callable(setter):
                    setter(False)
                    return f"已快速隐藏 **{display}**。如果需要彻底删除，我可以在后续执行队列中继续清理。"
                return None

            if scale_factor is not None:
                current = [float(v) for v in actor.get_scale()]
                new_scale = [round(max(0.02, v * scale_factor), 4) for v in current[:3]]
                actor.set_scale(new_scale)
                changed_parts.append(f"缩放调整为 {new_scale}")

            rotation_delta = self._parse_fast_rotation_delta(user_text)
            if rotation_delta is not None:
                current_rot = [float(v) for v in actor.get_rotation()]
                while len(current_rot) < 3:
                    current_rot.append(0.0)
                current_rot[1] = round(current_rot[1] + rotation_delta, 3)
                actor.set_rotation(current_rot[:3])
                changed_parts.append(f"旋转调整为 {current_rot[:3]}（弧度）")

            color = self._parse_fast_color(user_text)
            if color is not None:
                label, rgb = color
                if self._try_apply_actor_color(actor, rgb):
                    changed_parts.append(f"颜色调整为{label}")

            boundary_style = self._try_fast_boundary_style_edit(user_text, actor)
            if boundary_style:
                changed_parts.extend(boundary_style)

            delta = self._parse_fast_position_delta(user_text, actor)
            if delta is not None:
                current_pos = [float(v) for v in actor.get_position()]
                new_pos = [
                    round(current_pos[0] + float(delta[0]), 4),
                    round(current_pos[1] + float(delta[1]), 4),
                    round(current_pos[2] + float(delta[2]), 4),
                ]
                actor.set_position(new_pos)
                changed_parts.append(f"位置调整为 {new_pos}")

            needs_grounding = scale_factor is not None or any(
                k in user_text for k in ("穿模", "贴地", "落地", "抬高", "底座", "重叠", "太挤", "碰撞")
            )
            if delta is not None:
                needs_grounding = True
            if not needs_grounding:
                if not changed_parts:
                    return None
                pos = [round(float(v), 3) for v in actor.get_position()]
                scale = [round(float(v), 3) for v in actor.get_scale()]
                return f"已快速调整 **{display}**：{'；'.join(changed_parts)}。当前位置 {pos}，缩放 {scale}。"

            try:
                from plugins.AITool.cai_extensions.mcp.tools.transform_grounding import (
                    resolve_actor_overlaps,
                    snap_actor_to_ground,
                )
            except Exception:  # noqa: BLE001
                from ..mcp.tools.transform_grounding import (  # type: ignore
                    resolve_actor_overlaps,
                    snap_actor_to_ground,
                )

            snapped = snap_actor_to_ground(actor)
            if snapped is not None:
                changed_parts.append(
                    "已自动贴地"
                )
            obstacles = [other for other in actors if other is not actor]
            repair = resolve_actor_overlaps(actor, obstacles, max_iterations=24)
            if repair.get("changed"):
                changed_parts.append("已避让重叠")
            if repair.get("remaining_overlap"):
                changed_parts.append("仍检测到局部重叠，请确认是否允许进一步移开")

            if not changed_parts:
                return None
            pos = [round(float(v), 3) for v in actor.get_position()]
            scale = [round(float(v), 3) for v in actor.get_scale()]
            return f"已快速调整 **{display}**：{'；'.join(changed_parts)}。当前位置 {pos}，缩放 {scale}。"
        except Exception as exc:  # noqa: BLE001
            logger.warning("[MasterAgent] fast edit failed for %s: %s", name, exc)
            return None

    def _try_fast_boundary_style_edit(self, user_text: str, actor: Any) -> list[str]:
        name = str(getattr(actor, "name", "") or "")
        if self._canonical_edit_actor_name(name) != "__terrain_boundary":
            return []
        text = str(user_text or "")
        if not any(k in text for k in ("低矮", "矮一点", "藤蔓", "木栏", "围栏", "栅栏", "边界", "不自然", "奇怪")):
            return []
        changed: list[str] = []
        if any(k in text for k in ("低矮", "矮一点", "太高", "别太高")):
            try:
                current = [float(v) for v in actor.get_scale()]
                while len(current) < 3:
                    current.append(1.0)
                new_scale = [
                    round(max(0.02, current[0]), 4),
                    round(min(max(0.02, current[1]), 0.55), 4),
                    round(max(0.02, current[2]), 4),
                ]
                actor.set_scale(new_scale[:3])
                changed.append(f"边界高度缩放调整为 {new_scale[:3]}")
            except Exception:
                logger.debug("[MasterAgent] boundary scale adjustment unavailable", exc_info=True)
        if any(k in text for k in ("藤蔓", "木栏", "木质", "温暖", "自然")):
            rgb = [0.34, 0.45, 0.18] if "藤蔓" in text else [0.42, 0.25, 0.12]
            if self._try_apply_actor_color(actor, rgb):
                changed.append("边界颜色调整为自然木藤色")
        if not changed:
            changed.append("已定位系统地形边界；当前工具未提供材质替换能力，已避免误改其他物体")
        return changed

    def _handle_edit(self, user_text: str, messages: List[str]) -> str:
        """编辑已有物体（增删改移缩放）→ 委托菜包 agentic 通道执行。

        菜包通道（_cai_app.chat → handle_integrated_entrance_stream → stream_agent）是
        验证过的 agentic 工具循环，引擎工具（set_actor_transform/remove_model 等）已全
        注册进它的 ToolRegistry。这里只负责：① 给它真实 actor 列表（解决 __shell_ 名字
        映射）；② 用【执行框架】prompt（而非聊天人设）让它真去调工具，不退化成闲聊。
        """
        # 1. 取真实 actor 列表（含 __shell_，排除 __room_/__interior_ 基础设施）
        try:
            from CoronaCore.core.managers import scene_manager
            sc = scene_manager.get("")
            if sc is None:
                routes = scene_manager.list_all()
                sc = scene_manager.get(routes[0]) if routes else None
        except Exception as e:
            logger.warning("[MasterAgent] edit: 无法访问场景: %s", e)
            sc = None
        if sc is None:
            return "⚠️ 当前没有可编辑的场景，请先生成一个场景。"

        editable_actors = []
        actors_lines = []
        for a in sc.get_actors():
            nm = a.name
            # 基础设施 actor（地形/草原/盒子/内皮地面）默认不进 AI 编辑列表（选项 B）：
            # 草原由建筑足迹自动派生（terrain=platform_radius×8），手动改它既歧义
            # （__room_terrain 地形 mesh + __terrain_grass 草簇是两个 actor）又跟派生冲突。
            # 例外：__terrain_boundary 是用户在 F5 中会直接指出的系统边界，允许低风险 grounding/高度/颜色调整。
            if (
                    (nm.startswith("__room_") or nm.startswith("__interior_") or nm.startswith("__terrain_"))
                    and self._canonical_edit_actor_name(nm) != "__terrain_boundary"
            ):
                continue
            editable_actors.append(a)
            try:
                pos = [round(v, 2) for v in a.get_position()]
                scl = [round(v, 2) for v in a.get_scale()]
                rot = [round(v, 1) for v in a.get_rotation()]
                actor_kind = "system_boundary" if self._canonical_edit_actor_name(nm) == "__terrain_boundary" else "editable"
                actors_lines.append(f"  - {nm}: kind={actor_kind} pos={pos} scale={scl} rot={rot}")
            except Exception:
                actors_lines.append(f"  - {nm}")
        if not actors_lines:
            return "⚠️ 场景里没有可编辑的物体。"

        if self._looks_like_fast_edit_request(user_text) and self._pick_edit_actor(user_text, editable_actors) is None:
            return self._candidate_actor_reply(editable_actors)

        fast_reply = self._try_fast_transform_edit(user_text, editable_actors)
        if fast_reply:
            logger.info("[MasterAgent] edit → fast transform path")
            return fast_reply

        # 2. 用执行框架 prompt 委托菜包 agentic 通道（它自主调引擎工具）
        actors_text = "\n".join(actors_lines)
        system = self._EDIT_EXEC_SYSTEM_PROMPT.format(actors=actors_text)
        logger.info("[MasterAgent] edit → 委托菜包 agentic 通道 (%d 个可编辑物体)", len(actors_lines))
        reply = self._call_caiapp(system, [f"用户: {user_text}"])
        return reply or "[场景设计大师] ✅ 已处理你的调整请求。"

    def _handle_scene(self, user_text: str, persona: str, messages: List[str],
                      force_compose: bool = False) -> str:
        specialist = self._router.route(persona)
        logger.info("[MasterAgent] scene → %s: %s (force_compose=%s)",
                    specialist.key, user_text[:80], force_compose)

        style_bible = dict(self._global_style)
        if specialist.style_bible:
            style_bible.update(specialist.style_bible)

        if specialist.key == "tidying":
            user_text = self._enhance_tidying(user_text, messages)

        scene_state = {
            "metadata": {"room_size": [5, 3, 3], "scene_name": "lanchat_scene", "style_bible": style_bible},
            "intermediate": {"style_bible": style_bible},
        }
        lanchat_context = self._extract_lanchat_context(messages)
        if lanchat_context:
            scene_state["metadata"].update(lanchat_context)

        # 场景组合（物品清单/整体布置）→ SceneComposer 批量建模+布局+导入
        # 判断同时看当前指令 + 历史消息：用户可能只说"生成3d模型/按清单生成"，
        # 而真正的物品清单在之前 AI 给出的消息里。
        # force_compose：外层已用开放式 LLM 判定是整场景生成（如"教堂""海底世界"），
        # 跳过这道关键词门，否则清单外场景词会被二次过滤掉、掉进单步编辑。
        from .scene_composer import is_compose_request
        compose_text = self._gather_compose_text(user_text, messages)
        if force_compose or is_compose_request(user_text) or is_compose_request(compose_text):
            if get_current_progress_sink() is not None:
                logger.info("[MasterAgent] LANChat compose request blocked from direct RoleAgent compose")
                return "这是生成类请求，但尚未开始生成。请先由房主确认当前方案，确认后系统会通过生成队列执行。"
            logger.info("[MasterAgent] scene → compose (整体场景组合)")
            return self._handle_scene_compose(user_text, messages, specialist, persona)

        # 复杂需求 → Multi-Step Planning 分解；简单指令 → 单步 Coordinator
        from .multi_step_planner import MultiStepPlanner
        if MultiStepPlanner.is_complex_task(user_text):
            logger.info("[MasterAgent] scene → multi-step (complex task)")
            return self._handle_scene_multistep(user_text, scene_state, style_bible, specialist)

        return self._handle_scene_single(user_text, scene_state, style_bible, specialist, messages)

    def _handle_scene_compose(self, user_text: str, messages: List[str],
                              specialist: "Specialist", persona: str = "") -> str:
        """整体场景组合：从清单/方案批量生成模型、布局、导入引擎。"""
        from .scene_composer import SceneComposer

        # 优先从最近对话中找完整清单文本（用户可能只说"按清单生成"）
        compose_text = self._gather_compose_text(user_text, messages)
        role_context = self._role_compose_context(persona)
        runtime = get_lanchat_scene_runtime()
        pending_context = runtime.consume_notes_for_prompt()
        if pending_context:
            compose_text = f"{compose_text}\n\n{pending_context}"
        if role_context:
            compose_text = f"{compose_text}\n\n## RoleAgent 软偏好\n{role_context}"
        image_url = self._extract_image_url(messages)

        composer = SceneComposer(room_size=[5.0, 3.0, 3.0], scene_name="lanchat_scene",
                                 max_items=self._scene_max_items)
        agent_name = self._extract_current_agent_name(messages) or specialist.name
        runtime.start_compose(agent_name, compose_text)
        try:
            result = composer.compose(
                compose_text,
                image_url=image_url,
                do_import=True,
                progress_sink=get_current_progress_sink(),
            )
        finally:
            runtime.end_compose(agent_name)

        # 记录到 GroupAgent
        for _ in result.get("items", []):
            self.group_agent.on_operation_event({"data": {"action": "add"}, "metadata": {}})

        tag = f"[{specialist.name}]"
        if result.get("error") and not result.get("imported"):
            return f"{tag} ⚠️ 场景组合失败：{result['error']}"

        extracted = result.get("extracted_count", 0)
        model_count = result.get("model_count", 0)
        imported = result.get("imported", [])
        failed = result.get("failed", [])
        truncated = result.get("truncated", 0)
        progress_events = result.get("progress_events") or []
        progress_timeline = result.get("progress_timeline") or []
        final_report_text = result.get("final_report_text") or ""
        vlm_review_text = result.get("vlm_review_text") or ""
        vlm_skipped = result.get("vlm_review_skipped") or []
        vlm_timed_out = result.get("vlm_review_timed_out") or []
        operation_count = int(result.get("operation_count") or 0)
        pending_tasks = result.get("pending_tasks") or []
        snapshot_path = result.get("zone_decompose_snapshot")

        lines = [f"{tag} 🏗️ 场景组合完成"]
        if role_context:
            role_name = role_context.splitlines()[0].replace("RoleAgent: ", "")
            lines.append(f"  • RoleAgent：{role_name}（软偏好已注入）")
        lines.append(f"  • 识别物体：{extracted} 个")
        if truncated > 0:
            lines.append(f"  • ⚠️ 单次生成上限 {self._scene_max_items} 个，本次先做前 {self._scene_max_items} 个（剩 {truncated} 个可稍后继续）")
        lines.append(f"  • 获取模型：{model_count} 个")
        lines.append(f"  • 导入引擎：{len(imported)} 个")
        classification_summary = str(result.get("element_classification_summary") or "").strip()
        if classification_summary:
            lines.append(f"  • 资源分类：{classification_summary}")
        room_budget = result.get("room_budget") or {}
        room_size = room_budget.get("room_size") if isinstance(room_budget, dict) else None
        if isinstance(room_size, list) and len(room_size) >= 3:
            lines.append(f"  • 室内盒子尺寸：{float(room_size[0]):.1f} x {float(room_size[1]):.1f} x {float(room_size[2]):.1f}")
        bounds_check = result.get("indoor_bounds_check") or {}
        if isinstance(bounds_check, dict) and bounds_check.get("checked"):
            clamped = len(bounds_check.get("clamped") or [])
            scaled = len(bounds_check.get("scaled") or [])
            unresolved = len(bounds_check.get("unresolved") or [])
            lines.append(f"  • 布局边界检查：已检查 {len(bounds_check.get('checked') or [])} 个，修正 {clamped} 个，缩放 {scaled} 个，待处理 {unresolved} 个")
        if result.get("progressive"):
            phases = result.get("phases_run") or []
            lines.append(f"  • 渐进阶段：{(' / '.join(phases)) if phases else '已启用'}")
        if progress_timeline:
            last_progress = progress_timeline[-1]
            user_progress = last_progress.get("user_message")
            if user_progress:
                lines.append(f"  • 阶段披露：{user_progress}")
            else:
                lines.append(f"  • 阶段披露：{last_progress.get('percent', 100)}%")
        if progress_events:
            lines.append(f"  • 最近进度：{progress_events[-1]}")
        if operation_count:
            lines.append(f"  • 捕获用户介入：{operation_count} 条")
        if pending_tasks:
            lines.append(f"  • 生成中吸收：{len(pending_tasks)} 条后续要求")
            lines.extend(self._format_pending_tasks_summary(pending_tasks))
        # 15a：shell 外壳建筑独立汇报（它不走家具路径，否则成败不可见）
        shell_placed = result.get("shell_placed", [])
        shell_failed = result.get("shell_failed", [])
        if shell_placed or shell_failed:
            lines.append(f"  • 外壳建筑：放置 {len(shell_placed)} 个" +
                         (f"，失败 {len(shell_failed)} 个" if shell_failed else ""))
        if imported:
            lines.append(f"\n✅ 已放入场景：{('、'.join(imported[:10]))}")
        if shell_placed:
            lines.append(f"🏛️ 外壳：{('、'.join(shell_placed[:5]))}")
        if failed:
            lines.append(f"⚠️ 未完成：{('、'.join(failed[:8]))}")
        if shell_failed:
            lines.append(f"⚠️ 外壳未完成：{('、'.join(shell_failed[:5]))}")
        if final_report_text:
            lines.append(f"\n🧭 最终检查：{final_report_text}")
        if vlm_review_text and vlm_review_text not in final_report_text:
            lines.append(f"👁️ VLM 外审：{vlm_review_text}")
        if vlm_skipped or vlm_timed_out:
            lines.append(
                f"👁️ VLM 跳过：{len(vlm_skipped)} 个，超时：{len(vlm_timed_out)} 个"
            )
        if snapshot_path:
            lines.append(f"🧪 F5 分解快照：{snapshot_path}")
        return "\n".join(lines)

    def _format_pending_tasks_summary(self, pending_tasks: List[Dict[str, Any]]) -> List[str]:
        applied: List[str] = []
        waiting: List[str] = []
        attention: List[str] = []
        for task in pending_tasks[:12]:
            text = str(task.get("text") or "").strip()
            if not text:
                continue
            status = str(task.get("status") or "")
            short = text.replace("\n", " ")[:36]
            if status.startswith("applied") or status in {"already_in_remaining_plan", "recorded_layout_constraint"}:
                applied.append(short)
            elif status in {"pending_next_generation", "queued_edit_or_waiting_for_actor", "pending_for_planner"}:
                waiting.append(short)
            elif "confirm" in status or status in {"recorded_no_matching_asset"}:
                attention.append(short)
        lines: List[str] = []
        if applied:
            lines.append(f"    - 已应用：{'；'.join(applied[:3])}")
        if waiting:
            lines.append(f"    - 已记录待补：{'；'.join(waiting[:3])}")
        if attention:
            lines.append(f"    - 需确认：{'；'.join(attention[:3])}")
        return lines

    def _role_compose_context(self, persona: str) -> str:
        """Resolve role persona into advisory compose context."""
        try:
            from .role_registry import resolve_role_template
            tpl = resolve_role_template(persona)
            return tpl.to_compose_context() if tpl is not None else ""
        except Exception as e:  # noqa: BLE001
            logger.debug("[MasterAgent] role compose context skipped: %s", e)
            return ""

    def _gather_compose_text(self, user_text: str, messages: List[str]) -> str:
        """收集用于组合的文本：清单 + 布局/风格描述 + 当前指令。

        不仅回溯数字清单，也抓取 AI 之前给出的完整设计方案（布局要点/风格/空间关系），
        让 compose_scene 的 LLM 布局节点拿到足够语义（"床头柜靠床、衣柜靠墙"等）。
        """
        import re as _re

        # 若当前指令本身就含清单 → 直接用
        if len(_re.findall(r"^\s*\d+[\.、)]", user_text, _re.MULTILINE)) >= 3:
            return user_text

        # 否则从历史里收集：清单文本 + 设计方案文本
        list_body = ""
        design_body = ""
        for msg in reversed(messages):
            if msg.startswith("[此前对话摘要]") or msg.startswith("[摘要]"):
                continue
            body = msg.split(": ", 1)[-1] if ": " in msg else msg
            body = body.strip()
            if not body or len(body) < 10:
                continue
            # 数字清单（≥3 项）= 物体目录
            if not list_body and len(_re.findall(r"^\s*\d+[\.、)]", body, _re.MULTILINE)) >= 3:
                list_body = body
                continue
            # 长文本含布局/风格/位置关系关键词 = 设计方案
            if not design_body and len(body) > 80:
                design_score = sum(kw in body for kw in (
                    "布局", "靠墙", "居中", "放置", "对称", "走道", "动线",
                    "风格", "色调", "氛围", "地面", "窗户", "床尾", "床头",
                    "左侧", "右侧", "旁边", "上方", "下方", "对面", "附近",
                ))
                if design_score >= 3:
                    design_body = body

        if list_body:
            parts = [f"## 物体清单\n{list_body}"]
            if design_body:
                parts.append(f"## 设计描述\n{design_body}")
            parts.append(f"## 用户指令\n{user_text}")
            result = "\n\n".join(parts)
            logger.info("[MasterAgent] compose: 拼装布局上下文 (list=%d, design=%d, total=%d)",
                        len(list_body), len(design_body), len(result))
            return result
        return user_text

    def _handle_scene_single(self, user_text: str, scene_state: Dict[str, Any],
                             style_bible: Dict[str, Any], specialist: "Specialist",
                             messages: List[str]) -> str:
        """单步场景编辑 — 含协同感知（意图预览 / 冲突检测 / 物体锁）。"""
        sender = self._extract_sender(messages)

        # 注入引擎场景中已有的物体列表（供 LLM 感知当前状态，实现渐进式交互）
        try:
            from CoronaCore.core.managers import scene_manager
            sc = scene_manager.get("")
            if sc is None:
                routes = scene_manager.list_all()
                sc = scene_manager.get(routes[0]) if routes else None
            if sc is not None:
                actors = [{"name": a.name, "position": list(a.get_position())}
                          for a in sc.get_actors()
                          if not a.name.startswith("__room_")]
                if actors:
                    scene_state.setdefault("intermediate", {})["locked_actors"] = actors
                    logger.info("[MasterAgent] scene: 注入 %d 个已有物体到 LLM 上下文", len(actors))
        except Exception:
            pass

        # 注入参考图 URL（用于 3D 模型生成）
        image_url = self._extract_image_url(messages)
        if image_url:
            scene_state.setdefault("intermediate", {})["reference_image_url"] = image_url
            logger.info("[MasterAgent] scene: reference image %r", image_url)

        # 注入手动文件路径（用户直接指定导入的模型文件）
        file_path = self._extract_file_path(user_text)
        if file_path:
            scene_state.setdefault("intermediate", {})["direct_model_path"] = file_path
            logger.info("[MasterAgent] scene: direct file path %r", file_path)

        result = self.coordinator.handle(user_text=user_text, scene_state=scene_state, style_bible=style_bible)

        intent = result.get("intent", {})
        spatial = result.get("spatial", {})
        target = intent.get("target", "")
        position = spatial.get("position")

        # 协同感知：检测是否与他人正在操作的预览位置冲突
        collab_warning = ""
        if position and target:
            conflict_user = self.collaboration.check_preview_collision(sender, position)
            if conflict_user:
                collab_warning = f"\n⚠️ {conflict_user} 正在附近操作，请注意协调。"
            # 广播本次操作意图（供他人感知）+ 短暂锁定目标物体
            action = intent.get("action", "")
            self.collaboration.broadcast_intent(
                sender,
                tooltip=f"{action} {target}",
                preview_position=position,
                status="placing_object" if action == "add" else "moving",
            )
            if action in ("move", "modify", "delete"):
                self.collaboration.lock_object(target, sender, operation=action)

        # 记录操作到 GroupAgent (用于风格巡检)
        self.group_agent.on_operation_event({
            "data": {"action": intent.get("action"), "target": target},
            "metadata": {},
        })

        reply = self._format_scene(result, specialist) + collab_warning

        # 记录本轮对话到记忆单例（供后续记忆增强 / 上下文回忆）
        try:
            self.coordinator._memory_for_scene(scene_state).record_conversation(user_text, reply)
        except Exception as e:
            logger.warning("[MasterAgent] record_conversation failed: %s", e)

        return reply

    def _handle_scene_multistep(self, user_text: str, scene_state: Dict[str, Any],
                                style_bible: Dict[str, Any], specialist: "Specialist") -> str:
        """复杂需求 — 分解为子任务并逐步执行，返回计划+执行摘要。"""
        plan = self.planner.decompose(user_text, scene_state=scene_state)
        tasks = plan.get("tasks", [])
        if not tasks:
            # 分解失败，退化为单步
            result = self.coordinator.handle(user_text=user_text, scene_state=scene_state, style_bible=style_bible)
            return self._format_scene(result, specialist)

        run = self.planner.run_plan(scene_state=scene_state, style_bible=style_bible)

        # 每个子任务都记一次操作事件，便于风格守护累计
        for _ in tasks:
            self.group_agent.on_operation_event({"data": {"action": "multi_step"}, "metadata": {}})

        tag = f"[{specialist.name}]"
        lines = [f"{tag} 📐 复杂需求已分解为 {len(tasks)} 步:"]
        for i, t in enumerate(tasks, 1):
            lines.append(f"  {i}. {t.get('description', '?')}")
        completed = run.get("completed", 0)
        lines.append(f"\n✅ 已执行 {completed}/{len(tasks)} 步")
        if plan.get("analysis"):
            lines.append(f"\n💡 {plan['analysis']}")
        return "\n".join(lines)

    def _enhance_tidying(self, user_text: str, messages: List[str]) -> str:
        if len(user_text) > 10:
            return user_text
        return "检查当前场景的布局，发现任何不协调、重叠、悬空、比例异常的问题，并给出修正建议"

    # ── 普通聊天 ──────────────────────────────────────────────────

    def _handle_chat(self, persona: str, messages: List[str]) -> str:
        specialist = self._router.route(persona)
        system = self._build_chat_system(specialist, persona)

        # 记录到 GroupAgent (用于后续总结)
        for msg in messages:
            if msg.startswith("[摘要]") or msg.startswith("[此前"):
                continue
            if ": " in msg:
                user, text = msg.split(": ", 1)
                self.group_agent.on_chat_message(user, text)

        if self._fallback_chat:
            return self._fallback_chat(system, messages)
        return self._call_caiapp(system, messages)

    def _build_chat_system(self, specialist: Specialist, persona: str = "") -> str:
        base = "你是场景设计助手。如果用户询问你的能力，引导他们使用场景指令或 /help 查看。回复简洁实用。"
        if specialist.key == "generalist":
            system = base + "\n\n你拥有全风格设计能力。"
        else:
            system = specialist.inject_prompt(base)
        # T-2.4：role 注入说话风格（⟦DECIDE:role-depth⟧=只影响 voice，不进 decompose）。
        # 未命中任何角色模板时原样返回，不注入人格（退化为通用助手）。
        try:
            from .role_registry import inject_persona_voice
            system = inject_persona_voice(system, persona)
        except Exception as e:  # noqa: BLE001
            logger.debug("[MasterAgent] role persona 注入跳过: %s", e)
        return system

    def _call_caiapp(self, system: str, messages: List[str]) -> str:
        try:
            from plugins.AITool.main import AITool
            from Quasar.cai.protocol.request import ChatRequest
            convo = "\n".join(messages)
            text = f"{system}\n\n以下是群聊上下文：\n{convo}\n\n请以你的身份回复最新消息。"
            req = ChatRequest.from_text(text=text, metadata={"skip_conversation_store": True})
            chunks = AITool._cai_app.chat(req)
            # CAIApp.chat 返回的是 build_success_response JSON 信封；必须先抽
            # llm_content[].part[].content_text，再做内部工具噪声过滤。
            clean = [_extract_cai_text_chunk(c) for c in chunks]
            reply = "".join(clean).strip()
            if reply:
                return reply
            # 全被过滤掉（只有工具结果、没自然语言）→ 给个通用成功反馈，不回 raw JSON
            return "✅ 已完成你的调整。"
        except Exception as e:
            logger.warning("[MasterAgent] CAIApp failed: %s, using fallback_chat", e)
            if self._fallback_chat:
                return self._fallback_chat(system, messages)
            return "🌐 网络好像不太稳定，请稍后再试～"

    # ── 格式化 ────────────────────────────────────────────────────

    def _format_scene(self, result: Dict[str, Any], specialist: Specialist) -> str:
        status = result.get("status", "unknown")
        intent = result.get("intent", {})
        spatial = result.get("spatial", {})
        exec_result = result.get("result", {})
        action = intent.get("action", "?")
        target = intent.get("target", "物体")
        tag = f"[{specialist.name}]"

        # 记忆提示（连续操作时的主动询问）+ 歧义候选选项
        memory_hint = result.get("memory_hint", "") or exec_result.get("memory_hint", "")
        mem_suffix = f"\n{memory_hint}" if memory_hint else ""

        if status == "executed":
            pos = spatial.get("position", [0, 0, 0])
            pos_str = f"({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})" if pos and len(pos) >= 3 else ""
            note = exec_result.get("note", "")
            model_info = f" [{note}]" if note else ""
            return f"{tag} ✅ 已{'放置' if action == 'add' else '移动'}「{target}」到 {pos_str}{model_info}{mem_suffix}"
        if status == "planned_only":
            pos = spatial.get("position", [0, 0, 0])
            pos_str = f"({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})" if pos and len(pos) >= 3 else ""
            note = exec_result.get('note', '引擎未连接')
            has_model = exec_result.get('model_path', '')
            if has_model:
                note += f" (模型已就绪: {has_model})"
            return f"{tag} 📋 已规划「{target}」位置 {pos_str}\n({note}){mem_suffix}"
        if status == "pending_user_approval":
            msg = exec_result.get("message", "需要确认")
            choices = exec_result.get("choices", [])
            lines = [f"{tag} ❓ {msg}"]
            if choices:
                lines.append("\n可选方案（回复编号或直接说明）：")
                for i, c in enumerate(choices, 1):
                    lines.append(f"  {i}. {c.get('label', '?')}")
            return "\n".join(lines) + mem_suffix
        if action == "question":
            return f"{tag} 🤔 请具体说明，例如: 在沙发旁边加个台灯"
        if status == "error":
            return f"{tag} ❌ {exec_result.get('error', '未知错误')}"
        return f"{tag} ⚠️ 操作未完成"

    # ── 辅助 ───────────────────────────────────────────────────────

    def _extract_trigger_text(self, messages: List[str]) -> str:
        if not messages:
            return ""
        for msg in reversed(messages):
            if msg.startswith("[此前对话摘要]") or msg.startswith("[摘要]"):
                continue
            text = msg.split(": ", 1)[-1] if ": " in msg else msg
            text = re.sub(r'@\w+\s*', '', text).strip()
            return text
        return messages[-1] if messages else ""

    def _extract_current_agent_name(self, messages: List[str]) -> str:
        """Read the explicitly injected current @agent context from orchestrator."""
        for msg in messages:
            m = re.search(r"本轮明确被 @ 的 AI 助手是：([^。\n]+)", str(msg or ""))
            if m:
                return m.group(1).strip()
        return ""

    def _extract_lanchat_context(self, messages: List[str]) -> Dict[str, str]:
        """Extract internal LANChat routing scope injected by the orchestrator."""
        context: Dict[str, str] = {}
        aliases = {
            "room_id": "room_id",
            "lanchat_room_id": "room_id",
            "plan_id": "plan_id",
            "seed_plan_id": "plan_id",
            "batch_id": "batch_id",
            "agent_id": "agent_id",
            "agent_name": "agent_name",
        }
        for msg in messages or []:
            text = str(msg or "")
            if "链路上下文" not in text:
                continue
            for key, value in re.findall(r"([a-zA-Z_][\w]*)=([^\s,，;；]+)", text):
                mapped = aliases.get(key)
                if mapped and value:
                    context[mapped] = value.strip()
        return context

    def _extract_sender(self, messages: List[str]) -> str:
        """从最近一条非摘要消息中提取发言人名字（用于协同锁/意图预览）。"""
        if not messages:
            return "user"
        for msg in reversed(messages):
            if msg.startswith("[此前对话摘要]") or msg.startswith("[摘要]"):
                continue
            if ": " in msg:
                return msg.split(": ", 1)[0].strip() or "user"
            return "user"
        return "user"

    def _extract_image_url(self, messages: List[str]) -> str:
        """从最近的消息中提取 fileid:// 或 http 图片 URL。

        用于场景编辑时作为 3D 生成的参考图。
        """
        import re as _re
        for msg in reversed(messages):
            if msg.startswith("[此前对话摘要]") or msg.startswith("[摘要]"):
                continue
            text = msg.split(": ", 1)[-1] if ": " in msg else msg
            # fileid://xxx
            m = _re.search(r'fileid://(\S+)', text)
            if m:
                return f"fileid://{m.group(1)}"
            # 效果图 URL
            m2 = _re.search(r'(https?://\S+\.(?:png|jpg|jpeg|webp|gif))', text, _re.IGNORECASE)
            if m2:
                return m2.group(1)
        return ""

    def _extract_file_path(self, text: str) -> str:
        """从用户消息中提取显式指定的模型文件路径（.obj/.glb/.fbx等）。

        用于"导入 F:\\path\\to\\model.obj"类指令——跳过搜索/生成，直接导入。
        """
        import re as _re, os as _os
        m = _re.search(r"((?:[A-Za-z]:[/\\]|/)[^\s]{3,}\.(?:obj|glb|gltf|fbx|dae|stl))",
                       text, _re.IGNORECASE)
        if m:
            p = m.group(1).replace("/", _os.sep).replace("\\", _os.sep)
            if _os.path.isfile(p):
                return p
        return ""


# ═══════════════════════════════════════════════════════════════════════════
# 工厂函数
# ═══════════════════════════════════════════════════════════════════════════

def create_master_agent(
    global_style: Dict[str, Any] = None,
    fallback_chat: Callable[[str, List[str]], str] = None,
    scene_max_items: int = 8,
) -> MasterAgent:
    """创建 MasterAgent — 替换 LANChat 的 _make_agent_ai_chat()。

    用法 (在 LANChat main.py 中):
        from cai_extensions.agent.agent_adapter import create_master_agent
        server._agent_runner = AgentRunner(ai_chat=create_master_agent())

    scene_max_items: 单次场景组合生成的物体数量上限（阶段测试期 4，省 hunyuan 消耗）。
    """
    return MasterAgent(fallback_chat=fallback_chat, global_style=global_style,
                       scene_max_items=scene_max_items)


def create_summary_agent(
    fallback_chat: Callable[[str], str] = None,
) -> SummaryAgent:
    """创建 SummaryAgent — 替换 LANChat 的 _make_summary_ai_chat()。

    用法 (在 LANChat main.py 中):
        from cai_extensions.agent.agent_adapter import create_summary_agent
        server._summary_service = SummaryService(ai_chat=create_summary_agent())
    """
    return SummaryAgent(fallback_chat=fallback_chat)


# 兼容旧接口
create_lanchat_adapter = create_master_agent
AgentAdapter = MasterAgent


__all__ = [
    "MasterAgent", "SummaryAgent", "AgentAdapter",
    "Specialist", "PersonaRouter",
    "create_master_agent", "create_summary_agent", "create_lanchat_adapter",
    "is_scene_command", "is_builtin_command", "SPECIALISTS",
]
