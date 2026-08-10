"""模型质量审查器 — 生成后逐个导入引擎 → 截图 → VLM 审查 → 修正 → 卸载。

与生成阶段解耦: 生成是纯 API 调用(不碰引擎), 审查是串行队列(全局锁保护),
避免并行截图导致引擎 GPU 管线死锁。

每条审查结果注入后续的 LLM 布局 prompt, 让布局知道旋转角/比例建议。
"""

from __future__ import annotations

import logging
import json
import os
import threading
from typing import Any, Dict, List, Optional

logger = logging.getLogger(__name__)

# 全局互斥锁: 同一时刻只允许一个审查会话, 防止截图竞态死锁
_review_lock = threading.Lock()

# 审查 VLM prompt
_REVIEW_SYSTEM_PROMPT = """你是 3D 模型质量检查员。检查单个模型的:
1. 朝向/旋转角是否合理 (椅子朝前, 灯朝上, 沙发靠背面朝外)
2. 比例是否正常 (不会过大或过小)
3. 是否有明显的生成缺陷 (残缺/变形/纹理错误)

输出 JSON:
{
  "overall": "PASS" | "FAIL",
  "rotation_correction": [rx, ry, rz],   // 需要的旋转修正(弧度), 不需要则为 [0,0,0]；例如 90度=1.5708
  "scale_correction": [sx, sy, sz],       // 需要的比例修正, 不需要则为 [1,1,1]
  "issues": ["问题描述"],
  "fix_suggestion": "修正建议 (给后续 LLM 布局用, 如: 旋转 90° 使其朝前)"
}"""


def _get_current_scene() -> Optional[Any]:
    try:
        try:
            from plugins.AITool.cai_extensions.mcp.tools.native_scene_state import (
                resolve_native_scene_value,
            )
        except ModuleNotFoundError:
            from ..mcp.tools.native_scene_state import resolve_native_scene_value  # type: ignore

        return resolve_native_scene_value("")
    except Exception as exc:
        logger.warning("[ModelReviewer] 无法获取当前场景: %s", exc)
        return None


def _capture_single_model(output_dir: str, model_name: str, tier: int = 99) -> Optional[str]:
    """Capture one model through the unified VLM review camera path."""
    scene = _get_current_scene()
    try:
        from .vlm_capture import capture_vlm_views
    except ImportError:
        from vlm_capture import capture_vlm_views
    result = capture_vlm_views(
        "",
        output_dir,
        actor_name=model_name,
        scope="actor",
        scene=scene,
    )
    if result.status == "success":
        logger.info(
            "[ModelReviewer] %s unified VLM 截图 %d/%d output_mode=%s",
            model_name,
            len(result.files),
            result.view_count,
            result.output_mode,
        )
        return result.output_dir
    logger.warning("[ModelReviewer] VLM 截图跳过 %s: %s", model_name, result.skipped_reason)
    return None


def _vlm_review_model(screenshot_dir: str, model_name: str, model_type: str) -> Dict[str, Any]:
    """对模型截图进行 VLM 审查, 返回结构化评审结果。"""
    from ..flows.scene_composition_workflow.helpers import get_tool, parse_review_result

    review_tool = get_tool("scene_rationality_review")
    if review_tool is None:
        logger.warning("[ModelReviewer] VLM 审查工具不可用, 跳过 %s", model_name)
        return {"overall": "SKIPPED", "rotation_correction": [0, 0, 0],
                "scale_correction": [1, 1, 1], "issues": [], "fix_suggestion": ""}

    scene_desc = (
        f"单模型质量审查: {model_name} (类型: {model_type})\n"
        f"检查旋转角、比例、缺陷。只输出 JSON。"
    )

    try:
        raw = review_tool.invoke({
            "output_dir": screenshot_dir,
            "scene_description": scene_desc,
            "max_images": 4,
        })
        parsed = parse_review_result(raw)
        # 提取 corrections 或 problem_actors 中的旋转/比例建议
        result = {
            "overall": parsed.get("overall", "PASS"),
            "rotation_correction": [0, 0, 0],
            "scale_correction": [1, 1, 1],
            "issues": parsed.get("issues", []),
            "fix_suggestion": "",
        }
        # 从 corrections 中提取旋转/比例
        corrections = parsed.get("corrections", []) or []
        for c in corrections:
            if c.get("rotation"):
                result["rotation_correction"] = c["rotation"]
            if c.get("scale"):
                result["scale_correction"] = c["scale"]
        # 从 suggestions 中提取修正建议
        suggestions = parsed.get("suggestions", []) or []
        if suggestions:
            result["fix_suggestion"] = "; ".join(suggestions[:2])
        return result
    except Exception as e:
        logger.warning("[ModelReviewer] VLM 审查异常 %s: %s", model_name, e)
        return {"overall": "ERROR", "rotation_correction": [0, 0, 0],
                "scale_correction": [1, 1, 1], "issues": [str(e)], "fix_suggestion": ""}


def review_single_model(
    model_path: str,
    model_name: str,
    model_type: str = "",
    output_base: str = "",
) -> Dict[str, Any]:
    """审查单个模型: 导入引擎 → 截图 → VLM → 记录修正 → 卸载。

    全局锁保护, 同一时刻只允许一个审查会话。
    返回审查结果, 注入后续布局 prompt。
    """
    import os as _os
    import tempfile as _tf

    with _review_lock:
        logger.info("[ModelReviewer] ====== 开始审查: %s ======", model_name)

        # 1. 导入引擎
        actor_name = None
        scene_name = ""
        try:
            scene = _get_current_scene()
        except Exception as e:
            logger.debug("[ModelReviewer] Python scene unavailable for %s, using native current scene: %s", model_name, e)
            scene = None

        if scene is not None:
            scene_name = str(getattr(scene, "route", "") or getattr(scene, "name", "") or "")
        if not _create_review_actor(scene_name, model_path, model_name):
            logger.warning("[ModelReviewer] native 导入失败: %s", model_name)
            return _empty_review()
        actor_name = model_name
        logger.info("[ModelReviewer] %s 导入成功", model_name)

        # 2. 截图
        tmp_dir = _os.path.join(_tf.gettempdir(), f"corona_review_{model_name}")
        screenshot_dir = _capture_single_model(tmp_dir, model_name)
        if not screenshot_dir:
            _remove_actor_safe(scene_name, model_name)
            return _empty_review()

        # 3. VLM 审查
        review = _vlm_review_model(screenshot_dir, model_name, model_type or model_name)

        # 4. 清理
        _remove_actor_safe(scene_name, model_name)
        # 清理截图临时文件
        try:
            import shutil
            shutil.rmtree(tmp_dir, ignore_errors=True)
        except Exception:
            pass

        review["model_name"] = model_name
        logger.info("[ModelReviewer] %s 审查完成: overall=%s rotation=%s scale=%s",
                    model_name, review["overall"],
                    review["rotation_correction"], review["scale_correction"])
        return review


def _empty_review() -> Dict[str, Any]:
    return {"overall": "SKIPPED", "rotation_correction": [0, 0, 0],
            "scale_correction": [1, 1, 1], "issues": [], "fix_suggestion": "",
            "model_name": ""}


def _create_review_actor(scene_name: str, model_path: str, model_name: str) -> bool:
    try:
        from api.editor_api import CoronaEditorApi

        actor_data = {
            "actor_name": model_name,
            "model_name": model_name,
            "object_id": model_name,
            "target": model_name,
            "skip_if_exists": True,
            "update_if_exists": True,
            "physics_enabled": False,
        }
        result = CoronaEditorApi.scene_tools.create_actor(
            scene_name,
            model_path,
            "model",
            actor_data,
        )
        return isinstance(result, dict) and result.get("status") != "error"
    except Exception as exc:
        logger.warning("[ModelReviewer] native 创建审查 actor 失败 %s: %s", model_name, exc)
        return False


def _remove_actor_safe(scene_name: str, name: str) -> None:
    try:
        from api.editor_api import CoronaEditorApi

        CoronaEditorApi.scene_tools.remove_actor(scene_name, name)
    except Exception as exc:
        logger.debug("[ModelReviewer] native 清理审查 actor 失败 %s: %s", name, exc)


def build_review_context(reviews: List[Dict[str, Any]]) -> str:
    """将审查结果构建为 LLM 布局 prompt 的上下文片段。

    注入到 compose_scene 的 prompt 中, 让 LLM 布局时考虑旋转角/比例建议。
    """
    if not reviews:
        return ""

    lines = ["\n## 模型审查结果 (布局时参考)"]
    for r in reviews:
        name = r.get("model_name", "?")
        rot = r.get("rotation_correction", [0, 0, 0])
        scl = r.get("scale_correction", [1, 1, 1])
        fix = r.get("fix_suggestion", "")
        issues = r.get("issues", [])

        parts = [f"- {name}:"]
        if any(v != 0 for v in rot):
            parts.append(f"旋转修正=[{rot[0]:.0f}, {rot[1]:.0f}, {rot[2]:.0f}]°")
        if any(v != 1 for v in scl):
            parts.append(f"比例修正=[{scl[0]:.2f}, {scl[1]:.2f}, {scl[2]:.2f}]")
        if fix:
            parts.append(fix)
        if issues:
            parts.append(f"问题: {'; '.join(issues[:2])}")
        lines.append("  ".join(parts))

    return "\n".join(lines)
