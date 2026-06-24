from __future__ import annotations

import concurrent.futures
import logging
import os
import threading
import time
from typing import Any, Dict, List

from Quasar.ai_tools.context import reset_current_session, set_current_session
from Quasar.ai_workflow.state import ModelRetrievalWorkflowState
from Quasar.ai_workflow.streaming import stream_output_node

from .formatters import NO_OUTPUT
from .helpers import normalize_object_id
from .progress import publish_user_progress
from .test_cases import get_test_case

logger = logging.getLogger(__name__)

_IMAGE_RETRY_HEARTBEAT_SECONDS = 60.0
_TEXT_TO_3D_HINTS = {"text_to_3d_preferred", "programmatic_boundary_preferred"}


def _lookup_cached_model(item_name: str) -> str:
    """Best-effort local model lookup used before image retry to avoid slow calls."""
    try:
        from .local_model_library import lookup_model

        return str(lookup_model(item_name) or "")
    except Exception as e:  # noqa: BLE001
        logger.debug("[Workflow][dispatch] 本地模型库查询失败（降级未命中）: %s", e)
        return ""


def _lookup_cached_image(item_name: str) -> str:
    """Best-effort local image lookup used before image compensation."""
    try:
        from .local_model_library import lookup_image

        return str(lookup_image(item_name) or "")
    except Exception as e:  # noqa: BLE001
        logger.debug("[Workflow][dispatch] 本地图片库查询失败（降级未命中）: %s", e)
        return ""


def _image_retry_max_workers(count: int) -> int:
    try:
        configured = int(os.getenv("CORONA_IMAGE_RETRY_MAX_WORKERS", "3") or "3")
    except ValueError:
        configured = 3
    return max(1, min(max(1, count), configured))


def _generation_mode_hint(elem: Dict[str, Any]) -> str:
    return str(elem.get("generation_mode_hint") or elem.get("resource_mode_hint") or "").strip()


def _text_to_3d_task(elem: Dict[str, Any], object_id: str) -> Dict[str, Any]:
    name = str(elem.get("item_name") or elem.get("name") or "").strip()
    prompt_text = str(elem.get("image_prompt") or name).strip()
    return {
        "item_name": name,
        "object_id": object_id,
        "image_url": f"__text_to_3d__:{prompt_text}",
        "image_prompt": prompt_text,
        "generation_mode_hint": _generation_mode_hint(elem),
    }


def _is_image_retry_fatal_error(exc: Exception) -> bool:
    text = str(exc)
    fatal_markers = (
        "未配置账号池",
        "旧客户端配置不完整",
        "无效的 URL",
        "Invalid Token",
        "request id",
        "api_key",
        "API Key",
    )
    return any(marker in text for marker in fatal_markers)


def _retry_failed_images(
    failed_elements: List[Dict[str, str]],
    session_id: str,
) -> Dict[str, str]:
    """对上游图片生成失败的元素进行一次补偿重试。"""
    from ..integrated_multi_scene_workflow.helpers import (
        extract_image_url,
        get_generate_image_tool,
    )

    image_tool = get_generate_image_tool()
    if not image_tool:
        logger.warning("[Workflow][dispatch] 图片生成工具不可用，无法补偿重试")
        return {}

    def _retry_one(elem: Dict[str, str]) -> tuple[str, str]:
        name = elem.get("item_name", "")
        prompt = elem.get("image_prompt", "")
        if not prompt:
            return name, ""
        token = set_current_session(session_id)
        try:
            raw_result = image_tool.invoke({"prompt": prompt})
            image_url = extract_image_url(raw_result)
            if image_url:
                logger.info("[Workflow][dispatch] %s 补偿图片生成成功", name)
                return name, image_url
            else:
                logger.warning("[Workflow][dispatch] %s 补偿图片生成结果为空", name)
        except Exception as e:
            logger.warning("[Workflow][dispatch] %s 补偿图片生成失败: %s", name, e)
        finally:
            reset_current_session(token)
        return name, ""

    def _probe_one(elem: Dict[str, str]) -> tuple[bool, tuple[str, str]]:
        name = elem.get("item_name", "")
        prompt = elem.get("image_prompt", "")
        if not prompt:
            return False, (name, "")
        token = set_current_session(session_id)
        try:
            raw_result = image_tool.invoke({"prompt": prompt})
            image_url = extract_image_url(raw_result)
            if image_url:
                logger.info("[Workflow][dispatch] %s 补偿图片探针成功", name)
                return False, (name, image_url)
            logger.warning("[Workflow][dispatch] %s 补偿图片探针结果为空", name)
        except Exception as e:
            if _is_image_retry_fatal_error(e):
                logger.warning(
                    "[Workflow][dispatch] 图片生成配置不可用，跳过本轮补偿重试并转文字直生 3D: %s",
                    e,
                )
                return True, (name, "")
            logger.warning("[Workflow][dispatch] %s 补偿图片探针失败: %s", name, e)
        finally:
            reset_current_session(token)
        return False, (name, "")

    recovered: Dict[str, str] = {}
    fatal, first_result = _probe_one(failed_elements[0])
    if fatal:
        return {}
    first_name, first_url = first_result
    if first_name and first_url:
        recovered[first_name] = first_url
    remaining = [
        elem for elem in failed_elements
        if elem.get("item_name", "") != first_name
    ]
    if not remaining:
        return recovered
    workers = _image_retry_max_workers(len(failed_elements))
    started_at = time.perf_counter()
    logger.info(
        "[Workflow][dispatch] 并发补偿图片: items=%s workers=%s",
        len(remaining),
        workers,
    )
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(_retry_one, elem) for elem in remaining]
        for future in concurrent.futures.as_completed(futures):
            name, image_url = future.result()
            if name and image_url:
                recovered[name] = image_url

    logger.info(
        "[Workflow][dispatch] 图片补偿完成: recovered=%s/%s elapsed=%.2fs",
        len(recovered),
        len(failed_elements),
        time.perf_counter() - started_at,
    )

    return recovered


def _start_image_retry_heartbeat(
    state: ModelRetrievalWorkflowState,
    *,
    total: int,
) -> threading.Event:
    stop_event = threading.Event()

    def _run() -> None:
        while not stop_event.wait(_IMAGE_RETRY_HEARTBEAT_SECONDS):
            publish_user_progress(
                state,
                "image_heartbeat",
                f"参考图片仍在准备中，本轮共 {total} 个物件；你可以继续补充要求。",
                progress=38,
            )

    threading.Thread(target=_run, daemon=True).start()
    return stop_event


@stream_output_node("integrated", NO_OUTPUT)
def dispatch_node(state: ModelRetrievalWorkflowState) -> Dict[str, Any]:
    """从第一步的输出中组装每个物体的检索/生成任务。"""
    metadata = state.get("metadata", {})
    global_assets = state.get("global_assets", {}) or {}

    if metadata.get("workflow_test"):
        test_case_key = metadata.get("workflow_test_case", "default")
        test_data = get_test_case(test_case_key)
        test_assets = test_data.get("global_assets", {})
        if test_assets:
            logger.info(
                "[Workflow][dispatch][TEST] 工作流测试模式，使用预定义 global_assets: "
                "test_case=%s",
                test_case_key,
            )
            global_assets = test_assets

    multi_scene = global_assets.get("multi_scene", {}) or {}

    approved = multi_scene.get("approved_elements") or state.get(
        "approved_elements",
        [],
    )
    generated_images: Dict[str, str] = multi_scene.get("generated_images") or state.get(
        "generated_images",
        {},
    )

    if not approved:
        return {"error": "无可处理的设计元素（第一步输出为空）"}

    tasks: List[Dict[str, Any]] = []
    failed_elements: List[Dict[str, str]] = []
    for idx, elem in enumerate(approved, start=1):
        name = elem.get("item_name", "")
        object_id = normalize_object_id(name, idx)
        mode_hint = _generation_mode_hint(elem)

        cached_model = _lookup_cached_model(name)
        if cached_model:
            cached_image = generated_images.get(name, "") or _lookup_cached_image(name)
            tasks.append(
                {
                    "item_name": name,
                    "object_id": object_id,
                    "image_url": cached_image or f"__local_model__:{name}",
                    "image_prompt": elem.get("image_prompt", ""),
                    "generation_mode_hint": mode_hint,
                    "local_model_cached": True,
                    "model_path": cached_model,
                }
            )
            logger.info(
                "[Workflow][dispatch] %s 命中本地模型库，跳过图片补偿: %s",
                name,
                cached_model,
            )
            continue

        if mode_hint in _TEXT_TO_3D_HINTS:
            tasks.append(_text_to_3d_task(elem, object_id))
            logger.info(
                "[Workflow][dispatch] %s 命中资源路由 guardrail=%s，跳过图生3D图片补偿，改用文字直生/程序化候选",
                name,
                mode_hint,
            )
            continue

        image_url = generated_images.get(name, "") or _lookup_cached_image(name)
        if not image_url:
            failed_elements.append(elem)
            continue
        tasks.append(
            {
                "item_name": name,
                "object_id": object_id,
                "image_url": image_url,
                "image_prompt": elem.get("image_prompt", ""),
                "generation_mode_hint": mode_hint,
            }
        )

    # 对图片生成失败的元素进行补偿重试
    if failed_elements:
        logger.info(
            "[Workflow][dispatch] %s 个元素无生成图片，尝试补偿重试",
            len(failed_elements),
        )
        publish_user_progress(
            state,
            "image_start",
            f"正在为 {len(failed_elements)} 个物件准备参考图片，期间可以继续补充要求。",
            progress=34,
            force=True,
        )
        session_id = str(state.get("session_id", "default") or "default")
        heartbeat_stop = _start_image_retry_heartbeat(state, total=len(failed_elements))
        try:
            recovered = _retry_failed_images(failed_elements, session_id)
        finally:
            heartbeat_stop.set()
        if recovered:
            publish_user_progress(
                state,
                "image_done",
                f"参考图片已准备 {len(recovered)}/{len(failed_elements)}，未完成的会降级为文字直生模型。",
                progress=42,
                force=True,
            )
        else:
            publish_user_progress(
                state,
                "image_degraded",
                "参考图片暂不可用，已切换为文字直生模型，不会卡住主流程。",
                progress=42,
                force=True,
            )
        for idx_offset, elem in enumerate(failed_elements, start=len(tasks) + 1):
            name = elem.get("item_name", "")
            image_url = recovered.get(name, "")
            object_id = normalize_object_id(name, idx_offset)
            if image_url:
                tasks.append(
                    {
                        "item_name": name,
                        "object_id": object_id,
                        "image_url": image_url,
                        "image_prompt": elem.get("image_prompt", ""),
                        "generation_mode_hint": _generation_mode_hint(elem),
                    }
                )
            else:
                # 文生图最终失败 → 降级为文字直生 3D（混元支持 text_to_3d，
                # 文字/图片二选一）。图片只是可选输入适配层，失败不该让整条链断。
                # image_url 用 __text_to_3d__: 前缀，generate_single_item 据此切文本模式；
                # retrieve_single_item 见此前缀会跳过图搜、直接转生成。
                tasks.append(_text_to_3d_task(elem, object_id))
                logger.info(
                    "[Workflow][dispatch] %s 文生图失败，降级为文字直生 3D（text_to_3d）",
                    name,
                )

    if not tasks:
        return {"error": "所有物体均无生成图片，无法进行模型检索"}

    logger.info("[Workflow][dispatch] 组装 %s 个检索/生成任务", len(tasks))
    publish_user_progress(
        state,
        "retrieval_prepare",
        f"已整理 {len(tasks)} 个资源请求，准备检索本地素材或生成模型。",
        progress=44,
        force=True,
    )
    return {
        "intermediate": {
            **state.get("intermediate", {}),
            "retrieval_tasks": tasks,
        },
    }
