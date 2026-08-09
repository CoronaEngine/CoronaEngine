from __future__ import annotations

import concurrent.futures
import logging
import threading
from typing import Any, Dict

from Quasar.ai_workflow.progress import publish_node_entries_event
from Quasar.ai_workflow.state import MultiSceneWorkflowState
from Quasar.ai_workflow.streaming import build_node_dialogue_entry, stream_output_node
from Quasar.ai_tools.context import reset_current_session, set_current_session

from .constants import IMAGE_MAX_WORKERS
from .formatters import NO_OUTPUT, format_generate_image_progress_parts
from .helpers import extract_image_url, get_generate_image_tool
from ..workflow_test_support import get_test_case
from ..model_retrieval_workflow.local_model_library import (
    lookup_image,
    lookup_model,
    save_image,
)

logger = logging.getLogger(__name__)

# 模块级图片缓存：同进程内复用已生成图片，减少 GRSAI API 调用
_image_cache: Dict[str, str] = {}
_image_cache_lock = threading.Lock()


def _normalize_cache_key(item_name: str) -> str:
    """归一化缓存键：去空白 + 小写。"""
    return (item_name or "").strip().lower()


def _get_cached_image(item_name: str) -> str | None:
    """三级缓存读取：内存 → 本地图片库 → None。命中本地库则回填内存。"""
    key = _normalize_cache_key(item_name)
    if not key:
        return None
    with _image_cache_lock:
        hit = _image_cache.get(key)
    if hit:
        return hit
    # 内存未命中 → 查本地图片库（跨进程持久化），命中回填内存
    disk = lookup_image(item_name)
    if disk:
        with _image_cache_lock:
            _image_cache[key] = disk
        return disk
    return None


def _set_cached_image(item_name: str, url: str) -> None:
    """写内存缓存的同时落盘到本地图片库（best-effort，失败不影响主链路）。"""
    key = _normalize_cache_key(item_name)
    if not key or not url:
        return
    with _image_cache_lock:
        _image_cache[key] = url
    save_image(item_name, url)


def _publish_generate_image_progress(
    state: MultiSceneWorkflowState,
    *,
    item_name: str,
    image_url: str,
    done_count: int,
    total_count: int,
    error_message: str = "",
) -> None:
    parts = format_generate_image_progress_parts(
        item_name=item_name,
        image_url=image_url,
        done_count=done_count,
        total_count=total_count,
        error_message=error_message,
    )
    if not parts:
        return

    entry = build_node_dialogue_entry(
        "integrated",
        parts,
        node_name="generate_images",
        function_id=state.get("function_id"),
    )
    publish_node_entries_event(
        str(state.get("session_id", "default") or "default"),
        "generate_images",
        [entry],
    )


@stream_output_node("integrated", NO_OUTPUT, node_name="generate_images")
def generate_images_node(state: MultiSceneWorkflowState) -> Dict[str, Any]:
    """并发生成所有审核通过元素的图片。"""
    metadata = state.get("metadata", {})

    if metadata.get("workflow_test"):
        test_case_key = metadata.get("workflow_test_case", "default")
        test_data = get_test_case(test_case_key)
        generated_images = test_data.get("generated_images")
        if isinstance(generated_images, dict) and generated_images:
            total_count = len(generated_images)
            for index, (item_name, image_url) in enumerate(
                generated_images.items(),
                1,
            ):
                _publish_generate_image_progress(
                    state,
                    item_name=item_name,
                    image_url=image_url,
                    done_count=index,
                    total_count=total_count,
                )
            logger.info(
                "[Workflow][generate_images][TEST] 工作流测试模式，使用预定义 generated_images: "
                "test_case=%s, count=%s",
                test_case_key,
                len(generated_images),
            )
            return {"generated_images": generated_images}

    approved = state.get("approved_elements", [])
    if not approved:
        logger.warning("[Workflow][generate_images] 无审核通过的元素")
        return {"generated_images": {}}

    image_tool = get_generate_image_tool()
    if not image_tool:
        logger.warning("[Workflow][generate_images] 图片生成工具不可用")
        return {"generated_images": {}}

    generated: Dict[str, str] = {}
    session_id = str(state.get("session_id", "default") or "default")

    def generate_one(element: Dict[str, str]) -> tuple[str, str, str]:
        name = element.get("item_name", "未命名")
        prompt = element.get("image_prompt", "")
        if not prompt:
            return name, "", "缺少图片生成提示词"

        # 本地模型库已有该物体 → 连图都不用生成（retrieve 顶部会按名命中库、与图无关），
        # 既省混元3D 又省文生图 token。返回空 URL，下游 dispatch/retrieve 照常按名查库。
        if lookup_model(name):
            logger.info(
                "[Workflow][generate_images] %s 已在本地模型库，跳过文生图",
                name,
            )
            return name, "", ""

        # 先查缓存：同物品名复用已生成图片
        cached = _get_cached_image(name)
        if cached:
            logger.info(
                "[Workflow][generate_images] %s 命中缓存，复用已有图片",
                name,
            )
            return name, cached, ""

        token = set_current_session(session_id)
        # 文生图对瞬时失败（read timeout / 空结果）做一次有界重试。
        # 混元链路里一个物体图片失败 = 该物体整条生成断掉（如客厅缺茶几），
        # 重试 1 次显著降低单点丢失；最坏耗时翻倍但不会无限拖。
        max_attempts = 2
        last_error = ""
        try:
            for attempt in range(1, max_attempts + 1):
                try:
                    raw_result = image_tool.invoke({"prompt": prompt})
                    image_url = extract_image_url(raw_result)
                    if not image_url:
                        last_error = "图片生成结果为空"
                        logger.warning(
                            "[Workflow][generate_images] %s 第 %d/%d 次结果为空%s",
                            name, attempt, max_attempts,
                            "，重试" if attempt < max_attempts else "，放弃",
                        )
                        continue
                    # 生成成功后写入缓存
                    _set_cached_image(name, image_url)
                    if attempt > 1:
                        logger.info(
                            "[Workflow][generate_images] %s 第 %d 次重试成功", name, attempt
                        )
                    return name, image_url, ""
                except Exception as e:
                    last_error = str(e)
                    logger.error(
                        "[Workflow][generate_images] %s 第 %d/%d 次生成失败: %s%s",
                        name, attempt, max_attempts, e,
                        "，重试" if attempt < max_attempts else "，放弃",
                    )
            return name, "", last_error
        finally:
            reset_current_session(token)

    max_workers = min(len(approved), IMAGE_MAX_WORKERS)
    completed_count = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as pool:
        futures = [pool.submit(generate_one, elem) for elem in approved]
        for future in concurrent.futures.as_completed(futures):
            try:
                name, url, error_message = future.result()
                if url:
                    generated[name] = url
                completed_count += 1
                _publish_generate_image_progress(
                    state,
                    item_name=name,
                    image_url=url,
                    done_count=completed_count,
                    total_count=len(approved),
                    error_message=error_message,
                )
            except Exception as e:
                logger.error("[Workflow][generate_images] 并发任务异常: %s", e)
                completed_count += 1
                _publish_generate_image_progress(
                    state,
                    item_name="未命名",
                    image_url="",
                    done_count=completed_count,
                    total_count=len(approved),
                    error_message=str(e),
                )

    logger.info(
        "[Workflow][generate_images] 成功生成 %s/%s 张图片",
        len(generated),
        len(approved),
    )
    return {"generated_images": generated}
