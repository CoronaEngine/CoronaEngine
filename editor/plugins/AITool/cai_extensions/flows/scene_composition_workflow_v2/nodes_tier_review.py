"""分层审查节点 — 每层独立截图 + VLM 审查 + 差量修正。

关键:
  - 每层放置后重新拍摄 (场景状态不同, 不能复用)
  - 差量修正: remove_model + import_model
  - 每层独立 retry 上限: MAX_TIER_RETRIES=2
  - VLM 直接输出结构化 problem_actors (GPT-5.5)
"""

from __future__ import annotations

import json as _json
import logging
import os
from pathlib import Path
from typing import Any, Dict, List, Optional

from Quasar.ai_workflow.streaming import stream_output_node

from ..scene_composition_workflow.formatters import NO_OUTPUT
from ..scene_composition_workflow.helpers import get_tool, parse_review_result

logger = logging.getLogger(__name__)

MAX_TIER_RETRIES = 2

# VLM issue → solver action 映射 (Week 2: 执行逻辑待接入 solver)
RULE_ACTION_MAP = {
    "too_far": "near_anchor",       # 太远 → 移到参照物附近
    "too_close": "near_anchor",     # 太近 → 移到参照物附近
    "overlap": "near_anchor",       # 重叠 → 偏移
    "floating": "bottom_align",     # 悬空 → 对齐底面
    "wrong_scale": "normalize_scale",  # 比例错 → 重新归一化
    "misaligned": "in_front",       # 未对齐 → 放在参照物前方
    "off_center": "center_under_group",  # 偏离中心 → 居中
}

# LLM 提取 problem_actors 的系统提示
_EXTRACT_ACTORS_SYSTEM_PROMPT = """你是场景分析助手。VLM 审查了 3D 场景并提出了自然语言反馈。

你的任务：从 VLM 的 issues 中提取有问题的物体，输出结构化 JSON。

**规则**:
1. actor 必须使用物体列表中的完整名称（一字不差）
2. 如果 VLM 用了简称，映射到完整名称（见下方示例）
3. 如果 VLM 没有明确指出某个物体有问题，返回空数组 []
4. issue 用英文标签: too_far | too_close | overlap | floating | wrong_scale | misaligned
5. reason 引用 VLM 的原文描述

**映射示例**:
- VLM 说"沙发距离过远" → actor: 物体列表中包含"沙发"的完整名称
- VLM 说"茶几位置不对" → actor: 物体列表中包含"茶几"的完整名称
- VLM 说"电视柜悬空" → actor: 物体列表中包含"电视柜"的完整名称

**注意**:
- 不要输出简称（❌ "沙发"）
- 必须输出列表中的完整名称（✅ "胡桃木新中式圈椅沙发"）
- 如果不确定对应哪个物体，不要猜测，返回空数组 []

只输出 JSON 数组，不要其他文字:
[{"actor": "完整物体名", "issue": "too_far", "reason": "VLM原文描述"}]"""


def _capture_for_review(state: Dict[str, Any], tier: int) -> Optional[str]:
    """Capture current scene through the unified 4-view base_color VLM path."""
    intermediate = state.get("intermediate", {})
    scene_json_path = intermediate.get("scene_json_path", "")
    if not scene_json_path:
        return None

    output_dir = str(Path(scene_json_path).parent / f"review_tier{tier}")
    os.makedirs(output_dir, exist_ok=True)

    try:
        from ...agent.vlm_capture import capture_vlm_views
    except ImportError:
        from cai_extensions.agent.vlm_capture import capture_vlm_views

    result = capture_vlm_views("", output_dir, scope="scene", timeout_sec=5.0)
    if result.status != "success":
        logger.warning("tier%d_review: unified VLM capture skipped: %s", tier, result.skipped_reason)
        return None
    logger.info("tier%d_review: unified VLM capture %d/%d → %s",
                tier, len(result.files), result.view_count, result.output_dir)
    return result.output_dir


def _fuzzy_match_actor_name(short_name: str, valid_names: set) -> Optional[str]:
    """模糊匹配简称 → 全名。如 '沙发' → '胡桃木新中式圈椅沙发'。
    多匹配时返回 None (无法确定 VLM 指的是哪个)，避免误判。
    """
    if not short_name or not valid_names:
        return None
    clean = short_name.strip()
    if clean in valid_names:
        return clean
    clean_lower = clean.lower()
    matches = [n for n in valid_names if clean_lower in n.lower()]
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        logger.warning(
            "_fuzzy_match: '%s' 匹配到多个: %s, 无法确定目标, 跳过",
            short_name, matches,
        )
    return None


# ===========================================================================
# 第二阶段: LLM 从 VLM 自然语言中提取结构化 problem_actors
# ===========================================================================

def _extract_problem_actors_with_llm(
    vlm_issues: List[str],
    tier_actor_names: List[str],
    timeout: float = 30.0,
) -> List[Dict[str, str]]:
    """用 LLM 从 VLM 的自然语言 issues 中提取结构化 problem_actors。

    VLM 擅长视觉理解但可能不输出结构化 actor 名 → LLM 做结构化提取。
    """
    if not vlm_issues or not tier_actor_names:
        return []

    actor_list = "\n".join(f"  - {n}" for n in tier_actor_names)
    issues_text = "\n".join(f"  - {i}" for i in vlm_issues)
    user_prompt = f"**VLM 反馈的问题**:\n{issues_text}\n\n**场景中的物体列表**:\n{actor_list}"

    try:
        from concurrent.futures import ThreadPoolExecutor, TimeoutError as FuturesTimeoutError
        from langchain_core.messages import HumanMessage, SystemMessage
        from Quasar.ai_models.base_pool.registry import get_chat_model

        llm = get_chat_model(temperature=0, request_timeout=15.0)

        def _do_call():
            return llm.invoke([
                SystemMessage(content=_EXTRACT_ACTORS_SYSTEM_PROMPT),
                HumanMessage(content=user_prompt),
            ])

        executor = ThreadPoolExecutor(max_workers=1)
        future = executor.submit(_do_call)
        try:
            response = future.result(timeout=timeout)
        except FuturesTimeoutError:
            executor.shutdown(wait=False, cancel_futures=True)
            logger.warning("_extract_problem_actors: LLM 超时")
            return []
        else:
            executor.shutdown(wait=False)

        text = (response.content if hasattr(response, "content") else str(response)).strip()
        if "```" in text:
            start = text.find("[")
            end = text.rfind("]")
            if start != -1 and end != -1:
                text = text[start: end + 1]
        result = _json.loads(text)
        if not isinstance(result, list):
            return []

        valid_names = set(tier_actor_names)
        validated = []
        for pa in result:
            if not isinstance(pa, dict):
                continue
            actor = pa.get("actor", "")
            if not actor:
                continue
            if actor in valid_names:
                validated.append({
                    "actor": actor,
                    "issue": pa.get("issue", "unknown"),
                    "reason": pa.get("reason", ""),
                })
            else:
                matched = _fuzzy_match_actor_name(actor, valid_names)
                if matched:
                    logger.info("_extract_problem_actors: fuzzy '%s' → '%s'", actor, matched)
                    validated.append({
                        "actor": matched,
                        "issue": pa.get("issue", "unknown"),
                        "reason": pa.get("reason", ""),
                    })
                else:
                    logger.info("_extract_problem_actors: actor '%s' 不在当前 tier, 跳过", actor)

        logger.info("_extract_problem_actors: extracted %d/%d valid actors", len(validated), len(result))
        return validated
    except Exception as e:
        logger.warning("_extract_problem_actors: 失败: %s", e)
        return []


# ===========================================================================
# 差量修正 (remove + import)
# ===========================================================================

def _generate_corrections_from_feedback(
    vlm_feedback: str,
    problem_actors: List[Dict[str, Any]],
    locked_actors: List[Dict[str, Any]],
    tier_items: List[Dict[str, Any]],
    room_size: List[float],
) -> List[Dict[str, Any]]:
    """VLM 未输出 corrections 时, 用文本 LLM 将自然语言反馈转为结构化修正。

    VLM 擅长视觉判断但可能不输出结构化 corrections,
    此函数作为 fallback: 文本 LLM 根据 VLM 的描述 + 当前坐标生成精确修正。
    """
    if not problem_actors:
        return []

    x_half = room_size[0] / 2
    z_half = (room_size[1] / 2) if len(room_size) > 1 else 1.5

    # 格式化当前场景状态
    layout_lines = []
    for a in locked_actors:
        name = a.get("name", a.get("actor_name", "?"))
        pos = a.get("position") or a.get("pos") or [0, 0, 0]
        scl = a.get("scale", [1, 1, 1])
        layout_lines.append(f"  {name}: pos={pos}, scale={scl}")
    layout_text = "\n".join(layout_lines) if layout_lines else "无"

    problem_text = "\n".join(
        f"  - {pa.get('actor', '?')}: {pa.get('issue', '?')} — {pa.get('reason', '')}"
        for pa in problem_actors
    )

    prompt = f"""你是室内设计修正专家。VLM 审查了场景后提出以下问题, 请为每个问题物体输出修正方案。

**房间**: {room_size[0]}×{room_size[1]}×{room_size[2] if len(room_size)>2 else 3}m, X∈[{-x_half},{x_half}], Z∈[{-z_half},{z_half}]

**当前场景**:
{layout_text}

**VLM 反馈的问题**:
{problem_text}

**输出 JSON 数组** (只包含需要修正的物体):
[
  {{
    "object_id": "物体名",
    "position": [x, y, z],
    "scale": [sx, sy, sz],
    "reason": "修正原因"
  }}
]

规则:
- position 必须在边界内, 落地 Y=0, 挂墙 Y≥1.5
- scale 根据相对大小调整: 台灯 0.3-0.4, 地毯 1.5-3.0, 大件保持 1.0
- 距离关系: 茶几在沙发前 0.5-0.8m, 灯具在参照物侧 0.3m"""

    try:
        from langchain_core.messages import HumanMessage, SystemMessage
        from Quasar.ai_models.base_pool.registry import get_chat_model

        llm = get_chat_model(temperature=0, request_timeout=30.0)
        response = llm.invoke([
            SystemMessage(content="你是室内设计修正专家。只输出 JSON 数组。"),
            HumanMessage(content=prompt),
        ])
        text = (response.content if hasattr(response, "content") else str(response)).strip()

        if "```" in text:
            start = text.find("[")
            end = text.rfind("]")
            if start != -1 and end != -1:
                text = text[start: end + 1]
        result = _json.loads(text)
        if isinstance(result, list) and result:
            logger.info("_gen_corrections: LLM 生成了 %d 个 corrections", len(result))
            return result
    except Exception as e:
        logger.warning("_gen_corrections: LLM 生成失败: %s", e)

    return []


def _apply_corrections(
    state: Dict[str, Any],
    tier: int,
    corrections: List[Dict[str, Any]],
    room_size: List[float],
) -> int:
    """执行 corrections: 只应用 scale (视觉可靠), 不应用 position (2D 截图不可靠)。

    Position 修正由 semantic retry + solver 负责。
    """
    if not corrections:
        return 0

    transform_tool = get_tool("set_actor_transform")
    if transform_tool is None:
        logger.warning("_apply_corrections: set_actor_transform 不可用")
        return 0

    applied = 0
    for corr in corrections:
        oid = corr.get("object_id", "")
        scl = corr.get("scale")
        reason = corr.get("reason", "")

        if not oid:
            logger.warning("_apply_corrections: 无效 correction: %s", corr)
            continue

        # 只应用 scale (VLM 能判断比例, 但判断不了精确 3D 位置)
        if not scl or len(scl) != 3:
            continue

        try:
            transform_tool.invoke({"actor_name": oid, "scale": scl})
            applied += 1
            logger.info("_apply_corrections: %s → scale=%s (%s)", oid, scl, reason)
        except Exception as e:
            logger.warning("_apply_corrections: %s scale 执行失败: %s", oid, e)

    if applied:
        logger.info("_apply_corrections: 成功执行 %d/%d 个修正", applied, len(corrections))
    return applied


def _apply_diff_correction(
    state: Dict[str, Any],
    tier: int,
    problem_actors: List[str],
    vlm_feedback: str,
) -> Dict[str, Any]:
    """差量修正: 删除问题 actor, 重新导入 (由下一轮 tier_place 完成 re-import)。

    这里只做 remove。re-import 由该层 tier_place 的 retry 路径处理:
    读取 state.intermediate.tier{tier}_retry_actors 和 tier{tier}_feedback。
    """
    intermediate = state.get("intermediate", {})
    scene_name = intermediate.get("scene_name", "composed_scene")

    # 只删除当前 tier 的 actor，不碰前层已锁定的
    tier_items = intermediate.get(f"tier{tier}_items", [])
    tier_actor_ids = {it.get("object_id", "") for it in tier_items}
    tier_actor_ids.update(it.get("name", "") for it in tier_items)
    tier_actor_ids.discard("")

    remove_tool = get_tool("remove_model")
    removed = set()
    ignored = []
    for actor_name in problem_actors:
        if actor_name not in tier_actor_ids:
            # VLM 报告了不属于当前 tier 的 actor（前一层的，已通过审查）
            ignored.append(actor_name)
            continue
        try:
            if remove_tool:
                remove_tool.invoke({"actor_name": actor_name, "scene_name": scene_name})
            removed.add(actor_name)
        except Exception as e:
            logger.warning("remove %s 失败: %s", actor_name, e)

    if ignored:
        logger.info("apply_diff: tier%d ignored %d non-tier actors: %s", tier, len(ignored), ignored)

    # 从 locked_actors/scene_actors 中移除已删除的 actor
    for key in ("locked_actors", "scene_actors"):
        actors = intermediate.get(key, [])
        if actors and removed:
            intermediate[key] = [
                a for a in actors
                if a.get("name", a.get("actor_name", "")) not in removed
            ]

    logger.info("apply_diff: tier%d removed %d/%d actors (ignored %d)", tier, len(removed), len(problem_actors), len(ignored))
    return state


# ===========================================================================
# Debug dump
# ===========================================================================

def _dump_review_debug(state, tier, retry_count, vlm_parsed, problem_names, feedback, screenshot_dir):
    """将 VLM 审查详情写入 debug JSON, 供数据分析。"""
    import json as _json
    from pathlib import Path as _Path
    from datetime import datetime as _dt

    intermediate = state.get("intermediate", {})
    metadata = state.get("metadata", {})
    scene_path = intermediate.get("scene_json_path", "")
    if not scene_path:
        return
    dump_dir = _Path(scene_path).parent
    dump_path = dump_dir / f"review_debug_t{tier}_r{retry_count}.json"
    try:
        debug = {
            "timestamp": _dt.now().isoformat(),
            "session": state.get("session_id", ""),
            "tier": tier,
            "retry": retry_count,
            "max_retries": MAX_TIER_RETRIES,
            "vlm_output": {
                "overall": vlm_parsed.get("overall"),
                "score": vlm_parsed.get("score"),
                "problem_actors": vlm_parsed.get("problem_actors", []),
                "issues": vlm_parsed.get("issues", []),
                "suggestions": vlm_parsed.get("suggestions", []),
                "details": vlm_parsed.get("details", {}),
            },
            "matched_problem_names": problem_names,
            "vlm_feedback_text": feedback,
            "tier_items": [
                {"object_id": it.get("object_id", ""), "name": it.get("name", "")}
                for it in intermediate.get(f"tier{tier}_items", [])
            ],
            "locked_actors": [
                {
                    "name": a.get("name", a.get("actor_name", "")),
                    "pos": a.get("position") or a.get("pos") or a.get("geometry", {}).get("pos"),
                    "aabb": a.get("aabb"),
                }
                for a in intermediate.get("locked_actors", [])
            ],
            "room_size": metadata.get("room_size"),
            "scene_name": metadata.get("scene_name", ""),
            "screenshot_dir": screenshot_dir,
        }
        dump_path.write_text(_json.dumps(debug, ensure_ascii=False, indent=2), encoding="utf-8")
        logger.info("review_debug: dumped tier%d_r%d → %s", tier, retry_count, dump_path)
    except Exception as e:
        logger.warning("review_debug: dump failed: %s", e)


# ===========================================================================
# 统一审查节点
# ===========================================================================

def _tier_review(state: Dict[str, Any], tier: int) -> Dict[str, Any]:
    """通用层级审查。

    输入 state 需含:
      intermediate.tier{tier}_items: 当前层的 items
      intermediate.locked_actors:   已导入的 actors

    输出到 intermediate:
      tier{tier}_review_result, tier{tier}_review_decision,
      tier{tier}_retry_count, tier{tier}_retry_actors, tier{tier}_feedback
    """
    intermediate = state.get("intermediate", {})
    tier_items = intermediate.get(f"tier{tier}_items", [])

    if not tier_items:
        logger.info("tier%d_review: 无物品, 跳过", tier)
        return {"intermediate": {
            f"tier{tier}_review_decision": "pass",
            "review_screenshot_dir": intermediate.get("review_screenshot_dir"),
        }}

    # 1. 截图 (每层独立)
    screenshot_dir = _capture_for_review(state, tier)

    # 2. VLM 审查
    review_tool = get_tool("scene_rationality_review")
    if review_tool is None or not screenshot_dir:
        logger.warning("tier%d_review: 无法审查, 跳过", tier)
        return {"intermediate": {
            f"tier{tier}_review_result": {"overall": "SKIPPED"},
            f"tier{tier}_review_decision": "pass",
            "review_screenshot_dir": screenshot_dir,
        }}

    # 构建审查描述: 带 focus/locked actors 隔离
    locked = intermediate.get("locked_actors", [])
    locked_names = ", ".join(
        a.get("name", a.get("actor_name", "")) for a in locked[:10] if a.get("name") or a.get("actor_name")
    )
    focus_names = ", ".join(it.get("name", it.get("object_id", "?")) for it in tier_items[:5])
    scene_desc_parts = [
        f"Tier{tier} 场景审查",
        f"【本次重点评估】: {focus_names}",
    ]
    if locked_names:
        scene_desc_parts.append(
            f"【已确定物体(不要报告问题)】: {locked_names}"
        )
    scene_desc_parts.append(
        "【输出规则】problem_actors 只填入本次重点评估的物体。"
        "已确定物体的建议写在 suggestions 中, 但不要放入 problem_actors。"
    )
    scene_desc = " | ".join(scene_desc_parts)

    try:
        raw = review_tool.invoke({
            "output_dir": screenshot_dir,
            "scene_description": scene_desc,
            "max_images": 4,
        })
        parsed = parse_review_result(raw)
    except Exception as e:
        logger.warning("tier%d_review: VLM 异常: %s", tier, e)
        parsed = {"overall": "ERROR", "issues": [str(e)], "problem_actors": []}

    if parsed.get("error"):
        logger.warning("tier%d_review: 审查错误: %s", tier, parsed["error"])
        return {"intermediate": {
            f"tier{tier}_review_result": parsed,
            f"tier{tier}_review_decision": "pass",
            "review_screenshot_dir": screenshot_dir,
        }}

    # 3. 决策
    overall = parsed.get("overall", "PASS")
    problem_actors_raw = parsed.get("problem_actors", []) or []

    retry_count = intermediate.get(f"tier{tier}_retry_count", 0)
    exceeded = retry_count >= MAX_TIER_RETRIES

    # 提取问题 actor 名称, 并模糊匹配回 object_id
    tier_items = intermediate.get(f"tier{tier}_items", [])
    known_ids = {it.get("object_id", "") for it in tier_items} | {it.get("name", "") for it in tier_items}
    problem_names = []
    for pa in problem_actors_raw:
        raw_name = pa.get("actor", "")
        if not raw_name:
            continue
        # 精确匹配优先
        if raw_name in known_ids:
            problem_names.append(raw_name)
            continue
        # 模糊匹配: 查找包含关系的 object_id
        matched = None
        for kid in known_ids:
            if kid and (kid in raw_name or raw_name in kid):
                matched = kid
                break
        if matched:
            problem_names.append(matched)
        else:
            problem_names.append(raw_name)  # 保留原始名称, retry 时会警告找不到
    feedback_lines = [
        f"- {pa.get('actor', '?')}: {pa.get('issue', '?')} ({pa.get('reason', '')})"
        for pa in problem_actors_raw
    ]
    feedback = "\n".join(feedback_lines) if feedback_lines else "VLM 认为布局需要调整"

    # 4a. 优先 VLM corrections: 直接设置坐标, 不再删除+重导入
    room_size = state.get("metadata", {}).get("room_size", [5, 3, 3])
    corrections_raw = parsed.get("corrections", []) or []
    corrections_from_vlm = bool(corrections_raw)  # VLM 直接输出的 correction
    corrections_applied = 0

    # VLM 未输出 corrections 但有问题反馈时, 用文本 LLM 生成 corrections
    if not corrections_raw and problem_actors_raw and overall == "NEEDS_IMPROVEMENT":
        locked_actors = intermediate.get("locked_actors", [])
        corrections_raw = _generate_corrections_from_feedback(
            vlm_feedback=feedback,
            problem_actors=problem_actors_raw,
            locked_actors=locked_actors,
            tier_items=tier_items,
            room_size=room_size,
        )
        if corrections_raw:
            parsed["corrections"] = corrections_raw
            corrections_from_vlm = False  # 文本 LLM 生成, 非 VLM 直接输出

    if corrections_raw and overall != "PASS":
        corrections_applied = _apply_corrections(state, tier, corrections_raw, room_size)
        if corrections_applied > 0:
            # _apply_corrections 只应用 scale, 不应用 position
            # position 始终由 semantic retry + solver 负责
            logger.info("tier%d_review: scale corrections 应用 %d 个, 仍执行 retry (位置由 solver 修正)",
                       tier, corrections_applied)
            # overall 保持 NEEDS_IMPROVEMENT → retry 触发

    if overall in ("PASS", "SKIPPED", "ERROR") or exceeded:
        decision = "pass"
        if exceeded and overall not in ("PASS", "SKIPPED"):
            logger.warning("tier%d_review: 已达重试上限 %d, 强制通过", tier, retry_count)
    else:
        decision = "fail"

    new_count = retry_count + 1 if decision == "fail" else retry_count

        # 5. debug dump: VLM 完整反馈 + 当前状态
    _dump_review_debug(state, tier, retry_count, parsed, problem_names, feedback, screenshot_dir)

    logger.info("tier%d_review: overall=%s decision=%s retry=%d/%d problems=%d corrections=%d",
                tier, overall, decision, new_count, MAX_TIER_RETRIES, len(problem_names), corrections_applied)

    # 4b. Rule Correction Fallback (Week 2: 诊断日志, 执行逻辑待接入 solver)
    if decision == "fail" and not corrections_applied and problem_actors_raw:
        for pa in problem_actors_raw:
            issue = pa.get("issue", "")
            action = RULE_ACTION_MAP.get(issue)
            if action:
                logger.info("[rule_map] %s: '%s' → solver action '%s' (Week 2 接入)",
                           pa.get("actor", "?"), issue, action)
            else:
                logger.info("[rule_map] %s: '%s' → 无匹配 action, 回退 diff",
                           pa.get("actor", "?"), issue)

    # 4c. corrections 未覆盖时, 回退差量修正: remove problem actors
    if decision == "fail" and problem_names:
        _apply_diff_correction(state, tier, problem_names, feedback)

    return {"intermediate": {
        f"tier{tier}_review_result": parsed,
        f"tier{tier}_review_decision": decision,
        f"tier{tier}_retry_count": new_count,
        f"tier{tier}_retry_actors": problem_names if decision == "fail" else None,
        f"tier{tier}_feedback": feedback if decision == "fail" else "",
        "review_result": parsed,  # v2→v1 兼容: output_result_node 读取此 key
        "review_screenshot_dir": screenshot_dir,
    }}


# ===========================================================================
# 对外节点 (每个节点直接调用 _tier_review)
# ===========================================================================

@stream_output_node("integrated", NO_OUTPUT)
def tier1_review_node(state: Dict[str, Any]) -> Dict[str, Any]:
    return _tier_review(state, 1)


@stream_output_node("integrated", NO_OUTPUT)
def tier2_review_node(state: Dict[str, Any]) -> Dict[str, Any]:
    return _tier_review(state, 2)


@stream_output_node("integrated", NO_OUTPUT)
def tier3_review_node(state: Dict[str, Any]) -> Dict[str, Any]:
    return _tier_review(state, 3)
