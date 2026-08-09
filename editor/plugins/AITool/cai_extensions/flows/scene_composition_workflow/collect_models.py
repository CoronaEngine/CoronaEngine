"""collect_models 节点 — 从 global_assets 提取模型检索结果，构建放置列表。"""

from __future__ import annotations

import logging
from typing import Any, Dict, List

from Quasar.ai_workflow.streaming import stream_output_node
from ..model_retrieval_workflow.helpers import resolve_model_file
from ..shared.asset_metadata import build_asset_metadata_batch

from .formatters import NO_OUTPUT

logger = logging.getLogger(__name__)


@stream_output_node("integrated", NO_OUTPUT)
def collect_models_node(state) -> Dict[str, Any]:
    """读取上游 model_retrieval 工作流存入的模型结果，转换为放置所需的 items 列表。"""
    metadata = state.get("metadata", {})

    # --test 模式：注入测试数据，绕过上游 model_retrieval 依赖
    if metadata.get("workflow_test"):
        from ..workflow_test_support import DEFAULT_MODELS, DEFAULT_PROMPT
        logger.info("collect_models: --test 模式，注入 %d 个默认测试模型", len(DEFAULT_MODELS))
        placement_items = [
            {
                "object_id": m["name"],
                "name": m["name"],
                "local_path": m["path"],
            }
            for m in DEFAULT_MODELS
        ]
        # 构建 asset_metadata (trimesh bbox)
        model_paths = [m["path"] for m in DEFAULT_MODELS]
        asset_meta = build_asset_metadata_batch(model_paths)

        # 若 state.prompt 为空，自动填入默认设计方案
        test_state_updates: Dict[str, Any] = {
            "intermediate": {
                "placement_items": placement_items,
                "total_models": len(DEFAULT_MODELS),
                "valid_models": len(DEFAULT_MODELS),
                "skipped_models": 0,
                "scene_name": metadata.get("scene_name", "test_scene"),
                "asset_metadata": asset_meta,
            },
            # test 模式下补齐 metadata，确保输出路径确定
            "metadata": {
                **metadata,
                "scene_name": metadata.get("scene_name", "test_scene"),
                "room_size": metadata.get("room_size", [5, 3, 3]),
            },
        }
        if not state.get("prompt"):
            test_state_updates["prompt"] = DEFAULT_PROMPT
        return test_state_updates

    global_assets = state.get("global_assets", {})
    model_retrieval = global_assets.get("model_retrieval", {})
    model_results: List[Dict[str, Any]] = model_retrieval.get("model_results", [])

    if not model_results:
        return {"error": "未找到模型检索结果，请先运行 /model_retrieval 工作流"}

    placement_items: List[Dict[str, Any]] = []
    for row in model_results:
        error = row.get("error")
        if error:
            logger.warning("跳过失败模型: %s (%s)", row.get("item_name", "?"), error)
            continue

        model_path = row.get("model_path", "")
        if not model_path:
            logger.warning("跳过缺少 model_path 的模型: %s", row.get("item_name", "?"))
            continue

        # 将相对路径解析为实际存在的绝对路径
        resolved_path = resolve_model_file(model_path)
        if not resolved_path:
            logger.warning(
                "跳过无法解析的模型路径 %s: %s",
                row.get("item_name", "?"),
                model_path,
            )
            continue

        item: Dict[str, Any] = {
            "object_id": row.get("object_id", row.get("item_name", "")),
            "name": row.get("item_name", ""),
            "file_name": row.get("item_name", ""),
            "local_path": resolved_path,
        }

        # 若上游提供了布局覆盖
        if row.get("pos"):
            item["pos"] = row["pos"]
        if row.get("rot"):
            item["rot"] = row["rot"]
        if row.get("scale"):
            item["scale"] = row["scale"]

        placement_items.append(item)

    if not placement_items:
        return {"error": "所有模型均失败，无法进行场景组合"}

    logger.info("collect_models: 收集到 %d 个可用模型", len(placement_items))

    model_paths = [it["local_path"] for it in placement_items if it.get("local_path")]
    asset_meta = build_asset_metadata_batch(model_paths)

    return {
        "intermediate": {
            "placement_items": placement_items,
            "total_models": len(model_results),
            "valid_models": len(placement_items),
            "skipped_models": len(model_results) - len(placement_items),
            "asset_metadata": asset_meta,
        },
    }
