from __future__ import annotations

import concurrent.futures
import logging
import os
import time
from typing import Any, Dict, List

from Quasar.ai_workflow.state import ModelRetrievalWorkflowState
from Quasar.ai_workflow.streaming import stream_output_node
from Quasar.ai_tools.context import reset_current_session, set_current_session

from .constants import GENERATION_MAX_WORKERS, GENERATION_MAX_RETRIES, GENERATION_RETRY_DELAY
from .formatters import (
    NO_OUTPUT,
    publish_node_progress,
)
from .progress import publish_user_progress
from .helpers import get_3d_generate_tool, parse_3d_result
from .local_model_library import save_model
from ..workflow_test_support import get_test_case

logger = logging.getLogger(__name__)

_MODEL_BATCH_WAIT_SECONDS = 15.0


def _generation_batch_count() -> int:
    try:
        return max(1, int(os.getenv("CORONA_HUNYUAN_GENERATION_BATCH_COUNT", "4") or "4"))
    except Exception:
        return 4


def _generation_batch_size() -> int:
    raw = os.getenv("CORONA_HUNYUAN_GENERATION_BATCH_SIZE", "").strip()
    if not raw:
        return 0
    try:
        return max(1, int(raw))
    except Exception:
        return 0


def _split_generation_batches(tasks: List[Dict[str, Any]]) -> List[List[Dict[str, Any]]]:
    items = list(tasks or [])
    if not items:
        return []
    explicit_size = _generation_batch_size()
    if explicit_size:
        return [items[i:i + explicit_size] for i in range(0, len(items), explicit_size)]
    batch_count = min(len(items), _generation_batch_count())
    base = len(items) // batch_count
    extra = len(items) % batch_count
    batches: List[List[Dict[str, Any]]] = []
    start = 0
    for idx in range(batch_count):
        size = base + (1 if idx < extra else 0)
        end = start + size
        batches.append(items[start:end])
        start = end
    return [batch for batch in batches if batch]


def _result_identity_key(item: Dict[str, Any]) -> tuple[str, str]:
    """为任务/结果生成稳定标识，便于重试回环保留已有结果。"""
    task_object_id = str(
        item.get("task_object_id") or item.get("object_id") or ""
    ).strip()
    item_name = str(item.get("item_name") or "").strip()
    return task_object_id, item_name


def _build_mock_generate_outputs(
    state: ModelRetrievalWorkflowState,
    retrieval_results: List[Dict[str, Any]],
    pending_generation: List[Dict[str, Any]],
) -> List[Dict[str, Any]] | None:
    """在 workflow_test 模式下根据测试样例直接构造生成阶段输出。"""
    metadata = state.get("metadata", {}) or {}
    if not metadata.get("workflow_test"):
        return None

    test_case = get_test_case(metadata.get("workflow_test_case", "default"))
    expected_results = test_case.get("expected_model_results", [])
    if not isinstance(expected_results, list) or not expected_results:
        return None

    retrieval_keys = {
        _result_identity_key(item)
        for item in retrieval_results
        if isinstance(item, dict)
    }
    pending_generation_by_key = {
        _result_identity_key(task): task
        for task in pending_generation
        if isinstance(task, dict)
    }
    is_retry_round = bool(pending_generation_by_key)

    generated_results: List[Dict[str, Any]] = []
    for expected in expected_results:
        key = _result_identity_key(expected)
        if key in retrieval_keys:
            continue
        if is_retry_round and key not in pending_generation_by_key:
            continue
        if expected.get("source") != "generation" and not expected.get("error"):
            continue

        row = dict(expected)
        retry_task = pending_generation_by_key.get(key)
        if retry_task:
            for field in (
                "retry_count",
                "review_reason",
                "task_object_id",
                "image_prompt",
                "search_error",
                "input_image_url",
                "image_url",
            ):
                if field in retry_task:
                    row[field] = retry_task[field]
        row["source"] = "generation"
        generated_results.append(row)

    if not generated_results:
        return None

    logger.info(
        "[Workflow][generate][TEST] 使用测试样例结果: 生成 %s",
        len(generated_results),
    )
    return sorted(generated_results, key=lambda item: item.get("task_index", 0))


def generate_single_item(
    task: Dict[str, Any],
    generate_tool: Any,
    session_id: str,
) -> Dict[str, Any]:
    """处理单个物体生成阶段，失败时自动重试。"""
    name = task["item_name"]
    object_id = task.get("object_id", "")
    image_url = task.get("input_image_url") or task.get("image_url", "")
    result: Dict[str, Any] = {
        "item_name": name,
        "object_id": object_id,
        "task_object_id": task.get("task_object_id", object_id),
        "task_index": task.get("task_index", 0),
        "input_image_url": image_url,
    }
    if "retry_count" in task:
        result["retry_count"] = task.get("retry_count", 0)
    if task.get("image_prompt"):
        result["image_prompt"] = task.get("image_prompt")

    search_error = str(task.get("search_error", "") or "").strip()

    if not generate_tool:
        error_message = "检索未命中且 3D 生成工具不可用"
        if search_error:
            error_message = f"检索失败且 3D 生成工具不可用: {search_error}"
        result.update({"source": "generation", "error": error_message})
        return result

    last_error = ""
    started_at = time.perf_counter()

    for attempt in range(1, GENERATION_MAX_RETRIES + 2):  # 1 initial + N retries
        if attempt > 1:
            delay = GENERATION_RETRY_DELAY * attempt
            logger.warning(
                "[Workflow][generate] %s 第 %s 次重试，等待 %.1fs...",
                name, attempt - 1, delay,
            )
            time.sleep(delay)

        logger.info("[Workflow][generate] %s 开始 3D 生成 (attempt %s/%s)",
                    name, attempt, GENERATION_MAX_RETRIES + 1)
        token = set_current_session(session_id)
        try:
            # 支持 text_to_3d 模式（image_url 以 __text_to_3d__: 开头时启用）
            if image_url and image_url.startswith("__text_to_3d__:"):
                prompt_text = image_url[len("__text_to_3d__:"):]
                tool_input = {
                    "mode": "text_to_3d",
                    "prompt": prompt_text or name,
                    "object_id": object_id,
                }
            else:
                tool_input = {
                    "mode": "image_to_3d",
                    "images": [image_url],
                    "object_id": object_id,
                    "prompt": name,  # 用于目录命名 (image_to_3d 模式下不发给 API)
                }
            raw = generate_tool.invoke(tool_input)
            model_info = parse_3d_result(raw)

            if model_info.get("error"):
                last_error = str(model_info.get("error", "生成结果解析为空"))
                logger.error(
                    "[Workflow][generate] %s 3D 生成失败 (attempt %s): %s",
                    name, attempt, last_error,
                )
                continue  # retry

            elapsed = time.perf_counter() - started_at
            result.update(
                {
                    "source": "generation",
                    "model_path": model_info.get("model_path", ""),
                    "parameter": model_info.get("parameter", {}),
                    "generation_elapsed_seconds": round(elapsed, 3),
                }
            )
            if search_error:
                result["search_error"] = search_error
            if attempt > 1:
                result["generation_attempts"] = attempt

            logger.info(
                "[Workflow][generate] %s 3D 模型生成完成: %s (attempt %s, elapsed=%.2fs)",
                name,
                model_info.get("model_path", ""),
                attempt,
                elapsed,
            )
            # 自动入库（幂等）：下一轮同名物体在 retrieve 顶部命中库 → 跳过混元3D。
            # best-effort，失败仅 warning，绝不影响本次返回。
            # 修复 P1 存库竞态：等混元 mesh 异步下载完成后再 resolve + 存库，避免扫到空目录。
            from .helpers import wait_mesh_then_resolve_model_file
            parameter = model_info.get("parameter", {})
            raw_model_path = model_info.get("model_path", "")
            final_model_path = wait_mesh_then_resolve_model_file(
                raw_model_path=raw_model_path,
                wait_object_id=str(parameter.get("object_id", object_id)),
                has_mesh_pending=bool(parameter.get("has_mesh_pending", False)),
                retry_times=3,
                retry_interval_seconds=0.2,
            )
            selected_model_path = final_model_path or raw_model_path
            try:
                from .runtime_assets import prepare_runtime_model_bundle

                runtime_bundle = prepare_runtime_model_bundle(selected_model_path)
                selected_model_path = runtime_bundle.runtime_model_path
                result["model_path"] = selected_model_path
            except Exception as exc:  # noqa: BLE001
                logger.warning(
                    "[Workflow][generate] %s runtime 素材准备失败，回退原模型: %s",
                    name,
                    exc,
                )
            save_model(name, selected_model_path)
            return result
        except Exception as e:
            last_error = str(e)
            logger.error(
                "[Workflow][generate] %s 3D 生成异常 (attempt %s): %s",
                name, attempt, e,
            )
            continue  # retry
        finally:
            reset_current_session(token)

    # All attempts exhausted
    elapsed = time.perf_counter() - started_at
    logger.error(
        "[Workflow][generate] %s 3D 生成最终失败，已重试 %s 次 (elapsed=%.2fs): %s",
        name, GENERATION_MAX_RETRIES, elapsed, last_error,
    )
    result.update({"source": "generation", "error": last_error})
    if search_error:
        result["search_error"] = search_error
    result["generation_attempts"] = GENERATION_MAX_RETRIES + 1
    return result


@stream_output_node(
    "integrated",
    NO_OUTPUT,
    node_name="generate",
)
def generate_node(state: ModelRetrievalWorkflowState) -> Dict[str, Any]:
    """执行生成阶段，并与检索命中结果合并。"""
    pending_generation = state.get("intermediate", {}).get("pending_generation", [])
    existing_results = state.get("model_results", [])

    if not isinstance(existing_results, list):
        existing_results = []

    retry_keys = {
        _result_identity_key(task)
        for task in pending_generation
        if isinstance(task, dict)
    }
    retained_results = [
        row
        for row in existing_results
        if isinstance(row, dict)
        and str(row.get("source", "") or "") != "pending_generation"
        and _result_identity_key(row) not in retry_keys
    ]
    retrieval_results = [
        row for row in retained_results if str(row.get("source", "") or "") == "retrieval"
    ]

    mock_generated = _build_mock_generate_outputs(
        state,
        retrieval_results,
        pending_generation,
    )
    if mock_generated is not None:
        total_count = len(mock_generated)
        for index, row in enumerate(mock_generated, 1):
            publish_node_progress(
                state,
                row,
                node_name="generate",
                done_count=index,
                total_count=total_count,
            )

        results = sorted(
            retained_results + mock_generated,
            key=lambda item: item.get("task_index", 0),
        )
        return {
            "model_results": results,
            "intermediate": {
                **state.get("intermediate", {}),
                "pending_generation": [],
            },
        }

    if not isinstance(pending_generation, list) or not pending_generation:
        return {
            "model_results": sorted(
                retained_results,
                key=lambda item: item.get("task_index", 0),
            ),
            "intermediate": {
                **state.get("intermediate", {}),
                "pending_generation": [],
            },
        }

    generate_tool = get_3d_generate_tool()
    if not generate_tool:
        logger.warning("[Workflow][generate] 3D 生成工具不可用，未命中项将返回错误")

    generated_results: List[Dict[str, Any]] = []
    completed_count = 0
    session_id = str(state.get("session_id", "default") or "default")

    six_view_images: Dict[str, Any] = {}

    generation_batches = _split_generation_batches(pending_generation)
    max_workers = min(
        max((len(batch) for batch in generation_batches), default=1),
        GENERATION_MAX_WORKERS,
    ) or 1
    logger.info(
        "[Workflow][generate] 3D generation batching: pending=%s batches=%s workers=%s limit=%s",
        len(pending_generation),
        len(generation_batches),
        max_workers,
        GENERATION_MAX_WORKERS,
    )
    if pending_generation:
        publish_user_progress(
            state,
            "model_start",
            f"开始生成 {len(pending_generation)} 个模型，将分 {len(generation_batches)} 批处理。",
            progress=54,
            force=True,
        )
    for batch_index, batch in enumerate(generation_batches, 1):
        batch_workers = min(len(batch), GENERATION_MAX_WORKERS) or 1
        batch_names = [str(task.get("item_name") or "?") for task in batch]
        logger.info(
            "[Workflow][generate] 3D generation batch %s/%s start: items=%s workers=%s names=%s",
            batch_index,
            len(generation_batches),
            len(batch),
            batch_workers,
            "、".join(batch_names[:6]),
        )
        publish_user_progress(
            state,
            "model_batch_start",
            f"第 {batch_index}/{len(generation_batches)} 批模型生成中：{'、'.join(batch_names[:4])}。",
            progress=55 + int((batch_index - 1) / max(1, len(generation_batches)) * 18),
            force=True,
        )
        with concurrent.futures.ThreadPoolExecutor(max_workers=batch_workers) as pool:
            futures = {
                pool.submit(generate_single_item, task, generate_tool, session_id): task
                for task in batch
            }
            pending_futures = set(futures)
            while pending_futures:
                done_futures, pending_futures = concurrent.futures.wait(
                    pending_futures,
                    timeout=_MODEL_BATCH_WAIT_SECONDS,
                    return_when=concurrent.futures.FIRST_COMPLETED,
                )
                if not done_futures:
                    publish_user_progress(
                        state,
                        "model_batch_heartbeat",
                        (
                            f"第 {batch_index}/{len(generation_batches)} 批模型仍在生成，"
                            f"已完成 {completed_count}/{len(pending_generation)}。"
                        ),
                        progress=55 + int((batch_index - 1) / max(1, len(generation_batches)) * 18),
                    )
                    continue
                for future in done_futures:
                    task = futures[future]
                    try:
                        result = future.result()
                    except Exception as e:
                        logger.error(
                            "[Workflow][generate] %s 生成任务异常: %s",
                            task.get("item_name", "?"),
                            e,
                        )
                        result = {
                            "item_name": task.get("item_name", "未知"),
                            "object_id": task.get("object_id", ""),
                            "task_index": task.get("task_index", 0),
                            "input_image_url": task.get("input_image_url", ""),
                            "source": "generation",
                            "error": str(e),
                        }

                    result.setdefault("generation_batch_index", batch_index)
                    result.setdefault("generation_batch_total", len(generation_batches))
                    generated_results.append(result)
                    completed_count += 1
                    publish_node_progress(
                        state,
                        result,
                        node_name="generate",
                        done_count=completed_count,
                        total_count=len(pending_generation),
                    )
        logger.info(
            "[Workflow][generate] 3D generation batch %s/%s done: completed=%s/%s",
            batch_index,
            len(generation_batches),
            completed_count,
            len(pending_generation),
        )
        publish_user_progress(
            state,
            "model_batch_done",
            f"第 {batch_index}/{len(generation_batches)} 批模型完成，累计 {completed_count}/{len(pending_generation)}。",
            progress=58 + int(batch_index / max(1, len(generation_batches)) * 18),
            force=True,
        )

    results = sorted(
        retained_results + generated_results,
        key=lambda item: item.get("task_index", 0),
    )

    logger.info(
        "[Workflow][generate] 完成: 检索命中 %s, 生成 %s, 失败 %s, 六视图 %s",
        sum(1 for row in results if row.get("source") == "retrieval"),
        sum(
            1
            for row in results
            if row.get("source") == "generation" and not row.get("error")
        ),
        sum(1 for row in results if row.get("error")),
        len(six_view_images),
    )

    return {
        "model_results": results,
        "six_view_images": six_view_images,
        "intermediate": {
            **state.get("intermediate", {}),
            "pending_generation": [],
        },
    }
