from __future__ import annotations

import logging
import os
from typing import Any, Dict, List

from Quasar.ai_workflow.state import ModelRetrievalWorkflowState
from Quasar.ai_workflow.streaming import stream_output_node

from .formatters import NO_OUTPUT, publish_node_progress
from .helpers import (
    get_store_tool,
    normalize_object_id,
    object_embedding_tools_enabled,
    parse_store_result,
    wait_for_pending_mesh,
)
from .progress import publish_user_progress
from ..workflow_test_support import get_test_case

logger = logging.getLogger(__name__)


def _collect_six_view_paths_from_dict(views: Any) -> List[str]:
    """从六视图字典中提取可用图片路径，按标准六视图顺序输出。"""
    if not isinstance(views, dict):
        return []

    ordered_keys = ("front", "back", "left", "right", "top", "bottom")
    ordered_paths: List[str] = []

    for key in ordered_keys:
        value = views.get(key)
        if isinstance(value, str) and value.strip() and os.path.exists(value):
            ordered_paths.append(str(value))

    # 兼容未来新增视角字段，避免遗漏可用图片。
    for key, value in views.items():
        if key in ordered_keys:
            continue
        if isinstance(value, str) and value.strip() and os.path.exists(value):
            ordered_paths.append(str(value))

    return ordered_paths


@stream_output_node("integrated", NO_OUTPUT, node_name="register")
def register_node(state: ModelRetrievalWorkflowState) -> Dict[str, Any]:
    """将生成成功的模型写入向量数据库。"""
    model_results = state.get("model_results", [])
    if not model_results:
        return {}

    embedding_enabled = object_embedding_tools_enabled()
    metadata = state.get("metadata", {}) or {}
    if metadata.get("workflow_test"):
        test_case = get_test_case(metadata.get("workflow_test_case", "default"))
        expected_results = test_case.get("expected_model_results", [])
        if isinstance(expected_results, list) and expected_results:
            expected_map = {
                (
                    str(item.get("item_name", "") or ""),
                    str(item.get("object_id", "") or ""),
                ): item
                for item in expected_results
                if isinstance(item, dict)
            }
            enriched_results: List[Dict[str, Any]] = []
            inserted_count = 0
            updated_count = 0
            failed_count = 0
            skipped_count = 0

            for row in model_results:
                key = (
                    str(row.get("item_name", "") or ""),
                    str(row.get("object_id", "") or ""),
                )
                merged = {**row, **expected_map.get(key, {})}
                status = str(merged.get("register_status", "") or "").lower()
                if status == "inserted":
                    inserted_count += 1
                elif status == "updated":
                    updated_count += 1
                elif status == "failed":
                    failed_count += 1
                else:
                    skipped_count += 1
                    merged.setdefault("register_status", "skipped")
                enriched_results.append(merged)

            logger.info(
                "[Workflow][register][TEST] 使用测试样例结果: inserted=%s, updated=%s, skipped=%s, failed=%s",
                inserted_count,
                updated_count,
                skipped_count,
                failed_count,
            )
            total_count = len(enriched_results)
            for progress_index, progress_item in enumerate(enriched_results, start=1):
                publish_node_progress(
                    state,
                    progress_item,
                    node_name="register",
                    done_count=progress_index,
                    total_count=total_count,
                )
            return {
                "model_results": enriched_results,
                "intermediate": {
                    **state.get("intermediate", {}),
                    "register_inserted": inserted_count,
                    "register_updated": updated_count,
                    "register_skipped": skipped_count,
                    "register_failed": failed_count,
                },
            }

    store_tool = get_store_tool()
    if not store_tool:
        if embedding_enabled:
            logger.warning("[Workflow][register] store_object 工具不可用，全部标记跳过")
            message = "模型入库工具不可用，本次仍会继续导入场景，后续复用缓存可能受影响。"
        else:
            logger.info("[Workflow][register] object embedding disabled，跳过向量入库")
            message = "模型向量入库已关闭，本次会继续导入场景，不再调用 Dashscope embedding。"
        publish_user_progress(
            state,
            "register_degraded",
            message,
            progress=78,
            force=True,
        )
    else:
        publish_user_progress(
            state,
            "register_start",
            f"正在稳定模型文件并写入本地库，共 {len(model_results)} 个。",
            progress=78,
            force=True,
        )

    inserted_count = 0
    updated_count = 0
    failed_count = 0
    skipped_count = 0
    enriched_results: List[Dict[str, Any]] = []
    total_count = len(model_results)

    for idx, row in enumerate(model_results, start=1):
        item = dict(row)

        if row.get("source") != "generation" or row.get("error") or not row.get("review_passed"):
            item["register_status"] = "skipped"
            skipped_count += 1
            enriched_results.append(item)
            publish_node_progress(
                state,
                item,
                node_name="register",
                done_count=len(enriched_results),
                total_count=total_count,
            )
            continue

        object_id = row.get("object_id") or normalize_object_id(
            row.get("item_name", ""),
            idx,
        )
        parameter = (
            row.get("parameter", {}) if isinstance(row.get("parameter"), dict) else {}
        )

        wait_for_pending_mesh(
            parameter=parameter,
            fallback_object_id=str(object_id),
            stage="register",
            wait_reason="用于入库图片稳定化",
        )

        six_view_paths = _collect_six_view_paths_from_dict(item.get("six_views_dict"))
        # 注：仅使用本轮 six_views_dict，不再从历史 state.six_view_images 回退查找
        # 降级链路：six_views_dict → preview_paths → input_image_url

        image_paths: List[str] = []
        preview_paths = parameter.get("preview_paths", [])
        if isinstance(preview_paths, list):
            item["preview_paths"] = [str(path) for path in preview_paths if path]
        else:
            item["preview_paths"] = []

        image_source = "input_image_url"

        # 图片入嵌入优先级：六视图 > 预览图 > 输入图。
        if six_view_paths:
            image_paths.extend(six_view_paths)
            image_source = "six_view"
        elif item["preview_paths"]:
            image_paths.extend(item["preview_paths"])
            image_source = "preview_paths"
        else:
            input_image_url = row.get("input_image_url", "")
            if input_image_url:
                image_paths.append(str(input_image_url))

        dedup_paths = list(dict.fromkeys(image_paths))
        logger.info(
            "[Workflow][register] %s 入库图片来源=%s, 数量=%s",
            object_id,
            image_source,
            len(dedup_paths),
        )

        item_name = row.get("item_name", "")
        global_assets = state.get("global_assets", {}) or {}
        multi_scene = global_assets.get("multi_scene", {}) or {}
        approved_elements = multi_scene.get("approved_elements") or state.get(
            "approved_elements",
            [],
        )
        image_prompt = (
            next(
                (
                    str(el.get("image_prompt", "") or "")
                    for el in approved_elements
                    if isinstance(el, dict) and el.get("item_name") == item_name
                ),
                f"{item_name or object_id or 'null'} 3D模型",
            )
            if len(dedup_paths) < 6
            else f"{item_name or object_id or 'null'} 3D模型 六视图"
        )

        if not store_tool:
            item["register_status"] = "skipped"
            item["register_error"] = (
                "object embedding disabled"
                if not embedding_enabled
                else "store_object 工具不可用"
            )
            skipped_count += 1
            item["object_id"] = object_id
            enriched_results.append(item)
            publish_node_progress(
                state,
                item,
                node_name="register",
                done_count=len(enriched_results),
                total_count=total_count,
            )
            continue

        try:
            raw_store_result = store_tool.invoke(
                {
                    "object_id": object_id,
                    "image_paths": dedup_paths,
                    "name": item_name,
                    "category": "generated_3d",
                    "description": image_prompt,
                }
            )

            parsed_store_result = parse_store_result(raw_store_result)
            register_status = parsed_store_result.get("register_status", "inserted")
            rowid = parsed_store_result.get("rowid")

            if parsed_store_result.get("error"):
                error_msg = parsed_store_result.get("error", "未知错误")
                item["register_status"] = "failed"
                item["register_error"] = error_msg
                failed_count += 1
            else:
                # 成功：直接使用返回的status
                item["register_status"] = register_status
                if rowid is not None:
                    item["register_rowid"] = rowid

                if register_status == "updated":
                    updated_count += 1
                else:
                    inserted_count += 1
        except Exception as e:  # noqa: BLE001
            item["register_status"] = "failed"
            item["register_error"] = str(e)
            failed_count += 1

        item["object_id"] = object_id
        enriched_results.append(item)
        publish_node_progress(
            state,
            item,
            node_name="register",
            done_count=len(enriched_results),
            total_count=total_count,
        )

    logger.info(
        "[Workflow][register] 完成: inserted=%s, updated=%s, skipped=%s, failed=%s",
        inserted_count,
        updated_count,
        skipped_count,
        failed_count,
    )
    publish_user_progress(
        state,
        "register_done",
        f"资源入库完成：新增 {inserted_count} 个，更新 {updated_count} 个，跳过 {skipped_count} 个，失败 {failed_count} 个。",
        progress=82,
        force=True,
    )

    return {
        "model_results": enriched_results,
        "intermediate": {
            **state.get("intermediate", {}),
            "register_inserted": inserted_count,
            "register_updated": updated_count,
            "register_skipped": skipped_count,
            "register_failed": failed_count,
        },
    }
