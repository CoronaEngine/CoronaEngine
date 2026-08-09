from __future__ import annotations

import concurrent.futures
import logging
import time
from typing import Any, Dict, List

from Quasar.ai_workflow.state import ModelRetrievalWorkflowState
from Quasar.ai_workflow.streaming import stream_output_node

from .constants import SEARCH_MAX_WORKERS
from .formatters import NO_OUTPUT, publish_node_progress
from .progress import publish_user_progress
from .helpers import get_search_tool, parse_search_result
from .local_model_library import lookup_model
from ..workflow_test_support import get_test_case

logger = logging.getLogger(__name__)


_SEARCH_CONFIG_ERROR_MARKERS = (
    "Invalid Token",
    "request id",
    "401",
    "Unauthorized",
    "api_key",
    "API Key",
    "未配置",
    "无效的 URL",
)


def _is_search_config_error(text: Any) -> bool:
    message = str(text or "")
    return any(marker in message for marker in _SEARCH_CONFIG_ERROR_MARKERS)


def _safe_search_error(text: Any) -> str:
    if _is_search_config_error(text):
        return "图搜/embedding 配置不可用（凭据或服务配置错误），已跳过检索并转入生成。"
    return str(text or "检索异常")


def _requires_image_search(task: Dict[str, Any]) -> bool:
    image_url = str(task.get("image_url", "") or "")
    if image_url.startswith("__text_to_3d__:") or image_url.startswith("__local_model__:"):
        return False
    if task.get("local_model_cached") and task.get("model_path"):
        return False
    return True


def _build_search_fast_fail_result(task: Dict[str, Any], reason: str) -> Dict[str, Any]:
    result: Dict[str, Any] = {
        "item_name": task.get("item_name", "未知"),
        "object_id": task.get("object_id", ""),
        "task_index": task.get("task_index", 0),
        "input_image_url": task.get("image_url", ""),
        "task_object_id": task.get("task_object_id", task.get("object_id", "")),
        "source": "pending_generation",
        "search_status": "search_unavailable_fatal",
        "search_error": reason,
    }
    image_prompt = task.get("image_prompt", "")
    if image_prompt:
        result["image_prompt"] = image_prompt
    return result


def _build_mock_retrieve_outputs(
    state: ModelRetrievalWorkflowState,
    tasks: List[Dict[str, Any]],
) -> Dict[str, Any] | None:
    """在 workflow_test 模式下根据测试样例直接构造检索阶段输出。"""
    metadata = state.get("metadata", {}) or {}
    if not metadata.get("workflow_test"):
        return None

    test_case = get_test_case(metadata.get("workflow_test_case", "default"))
    expected_results = test_case.get("expected_model_results", [])
    if not isinstance(expected_results, list) or not expected_results:
        return None

    task_map: Dict[str, Dict[str, Any]] = {}
    for task in tasks:
        task_map[str(task.get("item_name", ""))] = task
        task_map[str(task.get("object_id", ""))] = task

    retrieval_results: List[Dict[str, Any]] = []
    pending_generation: List[Dict[str, Any]] = []
    for index, expected in enumerate(expected_results, start=1):
        item_name = str(expected.get("item_name", "") or "")
        object_id = str(expected.get("object_id", "") or "")
        task = task_map.get(item_name) or task_map.get(object_id) or {}

        merged: Dict[str, Any] = {
            "item_name": item_name or task.get("item_name", "未知"),
            "object_id": object_id or task.get("object_id", ""),
            "task_index": expected.get("task_index", task.get("task_index", index)),
            "input_image_url": expected.get(
                "input_image_url",
                task.get("image_url", task.get("input_image_url", "")),
            ),
            **expected,
        }
        if task.get("image_prompt") and not merged.get("image_prompt"):
            merged["image_prompt"] = task.get("image_prompt")

        if merged.get("source") == "retrieval" and not merged.get("error"):
            retrieval_results.append(merged)
            continue

        pending_item = dict(merged)
        pending_item["source"] = "pending_generation"
        pending_item.setdefault("search_status", "mock_pending_generation")
        pending_generation.append(pending_item)

    logger.info(
        "[Workflow][retrieve][TEST] 使用测试样例结果: 命中 %s, 待生成 %s",
        len(retrieval_results),
        len(pending_generation),
    )
    return {
        "model_results": sorted(
            retrieval_results,
            key=lambda item: item.get("task_index", 0),
        ),
        "intermediate": {
            **state.get("intermediate", {}),
            "pending_generation": sorted(
                pending_generation,
                key=lambda item: item.get("task_index", 0),
            ),
        },
    }


def retrieve_single_item(task: Dict[str, Any], search_tool: Any) -> Dict[str, Any]:
    """处理单个物体检索阶段。"""
    name = task["item_name"]
    object_id = task.get("object_id", "")
    image_url = task["image_url"]
    image_prompt = task.get("image_prompt", "")

    result: Dict[str, Any] = {
        "item_name": name,
        "object_id": object_id,
        "task_index": task.get("task_index", 0),
        "input_image_url": image_url,
        "task_object_id": task.get("task_object_id", object_id),
    }
    if image_prompt:
        result["image_prompt"] = image_prompt

    # 本地模型库（最高优先级）：按名命中即复用，跳过图搜 + 混元3D 生成。
    # 放最顶 → 连"文生图失败降级成 __text_to_3d__"、图搜工具不可用的任务也能被库接住。
    # source="retrieval" 复用现有路由进 model_results，混元3D 不被调用。
    cached_dir = str(task.get("model_path", "") or "") if task.get("local_model_cached") else ""
    if not cached_dir:
        cached_dir = lookup_model(name)
    if cached_dir:
        result.update(
            {
                "source": "retrieval",
                "model_path": cached_dir,
                "search_status": "local_library",
                "distance": 0.0,
            }
        )
        logger.info("[Workflow][retrieve] %s 命中本地模型库，跳过生成: %s", name, cached_dir)
        return result

    if isinstance(image_url, str) and image_url.startswith("__local_model__:"):
        result.update(
            {
                "source": "pending_generation",
                "search_status": "local_library_miss",
            }
        )
        logger.info("[Workflow][retrieve] %s 本地模型标记失效，转生成队列", name)
        return result

    # text_to_3d 任务（文生图失败降级而来）：image_url 是文字标记，不能拿去图搜，
    # 直接转 pending_generation，让 generate_single_item 用 text_to_3d 模式接住。
    if isinstance(image_url, str) and image_url.startswith("__text_to_3d__:"):
        result.update(
            {
                "source": "pending_generation",
                "search_status": "text_to_3d",
            }
        )
        logger.info("[Workflow][retrieve] %s 文字直生任务，跳过图搜直接转生成", name)
        return result

    if not search_tool:
        result.update(
            {
                "source": "pending_generation",
                "search_status": "tool_unavailable",
            }
        )
        return result

    started_at = time.perf_counter()
    logger.info("[Workflow][retrieve] %s 开始检索", name)

    try:
        raw = search_tool.invoke(
            {
                "query_images": [image_url],
                "query_text": image_prompt,
                "top_k": 1,
            }
        )

        parsed_result = parse_search_result(raw)
        elapsed = time.perf_counter() - started_at

        error_msg = parsed_result.get("error", "")
        if error_msg:
            logger.warning(
                "[Workflow][retrieve] %s 检索失败，将降级生成: %s (elapsed=%.2fs)",
                name,
                error_msg or "未知错误",
                elapsed,
            )
            result.update(
                {
                    "source": "pending_generation",
                    "search_status": "error",
                    "search_error": _safe_search_error(error_msg or "检索异常"),
                    "search_error_raw": error_msg or "检索异常",
                }
            )
            return result

        hit = parsed_result.get("hit", False)
        best_match = parsed_result.get("best_match")

        if hit and best_match:
            image_paths = best_match.get("image_paths", [])
            if not isinstance(image_paths, list):
                image_paths = []
            result.update(
                {
                    "source": "retrieval",
                    "object_id": best_match.get("object_id", ""),
                    "name": best_match.get("name", ""),
                    "distance": best_match.get("distance", 0),
                    "model_path": best_match.get("model_path", ""),
                    "image_paths": image_paths,
                    "search_elapsed_seconds": round(elapsed, 3),
                }
            )
            logger.info(
                "[Workflow][retrieve] %s 检索命中: object_id=%s, distance=%.4f, elapsed=%.2fs",
                name,
                best_match.get("object_id"),
                best_match.get("distance", 0),
                elapsed,
            )
            return result

        # 未命中
        best_distance = best_match.get("distance", "N/A") if best_match else "N/A"
        logger.info(
            "[Workflow][retrieve] %s 检索未命中（最佳 distance=%s, elapsed=%.2fs）",
            name,
            best_distance,
            elapsed,
        )
        result.update(
            {
                "source": "pending_generation",
                "search_status": "miss",
                "best_distance": best_distance,
                "search_elapsed_seconds": round(elapsed, 3),
            }
        )
        return result
    except Exception as e:
        elapsed = time.perf_counter() - started_at
        logger.warning(
            "[Workflow][retrieve] %s 检索异常，将降级生成: %s (elapsed=%.2fs)",
            name,
            e,
            elapsed,
        )
        result.update(
            {
                "source": "pending_generation",
                "search_status": "error",
                "search_error": _safe_search_error(e),
                "search_error_raw": str(e),
                "search_elapsed_seconds": round(elapsed, 3),
            }
        )
        return result


@stream_output_node(
    "integrated",
    NO_OUTPUT,
    node_name="retrieve",
)
def retrieve_node(state: ModelRetrievalWorkflowState) -> Dict[str, Any]:
    """执行检索阶段，并输出待生成任务列表。"""
    tasks = state.get("intermediate", {}).get("retrieval_tasks", [])
    if not tasks:
        return {"error": "无检索/生成任务"}

    mock_outputs = _build_mock_retrieve_outputs(state, tasks)
    if mock_outputs is not None:
        mock_results = sorted(
            list(mock_outputs.get("model_results", []))
            + list(mock_outputs.get("intermediate", {}).get("pending_generation", [])),
            key=lambda item: item.get("task_index", 0),
        )
        total_count = len(mock_results)
        for index, row in enumerate(mock_results, 1):
            publish_node_progress(
                state,
                row,
                node_name="retrieve",
                done_count=index,
                total_count=total_count,
            )
        return mock_outputs

    search_tool = get_search_tool()
    if not search_tool:
        logger.warning("[Workflow][retrieve] 检索工具不可用，将全部走生成")
        publish_user_progress(
            state,
            "retrieval_degraded",
            "素材检索当前不可用，已切换为直接生成模型。",
            progress=48,
            force=True,
        )
    else:
        publish_user_progress(
            state,
            "retrieval_start",
            f"正在检索可复用素材，共 {len(tasks)} 个资源请求。",
            progress=46,
            force=True,
        )

    retrieval_results: List[Dict[str, Any]] = []
    pending_generation: List[Dict[str, Any]] = []

    indexed_tasks = [
        {**task, "task_index": task.get("task_index", index)}
        for index, task in enumerate(tasks, start=1)
    ]

    completed_count = 0

    def _record(retrieved: Dict[str, Any]) -> None:
        nonlocal completed_count
        if retrieved.get("source") == "retrieval":
            retrieval_results.append(retrieved)
        else:
            pending_generation.append(retrieved)
        completed_count += 1
        publish_node_progress(
            state,
            retrieved,
            node_name="retrieve",
            done_count=completed_count,
            total_count=len(indexed_tasks),
        )

    remaining_tasks = indexed_tasks
    if search_tool:
        for index, task in enumerate(indexed_tasks):
            retrieved = retrieve_single_item(task, search_tool)
            _record(retrieved)
            remaining_tasks = indexed_tasks[index + 1:]

            if not _requires_image_search(task):
                continue

            search_error = retrieved.get("search_error_raw", "") or retrieved.get("search_error", "")
            if _is_search_config_error(search_error):
                reason = _safe_search_error(search_error)
                logger.warning(
                    "[Workflow][retrieve] 图搜/embedding 配置错误，剩余 %s 个任务跳过检索直接生成: %s",
                    len(remaining_tasks),
                    reason,
                )
                for pending_task in remaining_tasks:
                    _record(_build_search_fast_fail_result(pending_task, reason))
                remaining_tasks = []
            break

    max_workers = min(len(remaining_tasks), SEARCH_MAX_WORKERS) or 1
    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as pool:
        futures = {
            pool.submit(retrieve_single_item, task, search_tool): task
            for task in remaining_tasks
        }
        for future in concurrent.futures.as_completed(futures):
            task = futures[future]
            try:
                retrieved = future.result()
            except Exception as e:
                logger.error(
                    "[Workflow][retrieve] %s 检索任务异常: %s",
                    task.get("item_name", "?"),
                    e,
                )
                retrieved = {
                    "item_name": task.get("item_name", "未知"),
                    "object_id": task.get("object_id", ""),
                    "task_index": task.get("task_index", 0),
                    "input_image_url": task.get("image_url", ""),
                    "task_object_id": task.get(
                        "task_object_id",
                        task.get("object_id", ""),
                    ),
                    "image_prompt": task.get("image_prompt", ""),
                    "source": "pending_generation",
                    "search_status": "error",
                    "search_error": str(e),
                }

            _record(retrieved)

    logger.info(
        "[Workflow][retrieve] 完成: 检索命中 %s, 待生成 %s",
        len(retrieval_results),
        len(pending_generation),
    )
    publish_user_progress(
        state,
        "retrieval_done",
        f"素材检索完成：命中 {len(retrieval_results)} 个，需要生成 {len(pending_generation)} 个。",
        progress=52,
        force=True,
    )

    return {
        "model_results": sorted(
            retrieval_results,
            key=lambda item: item.get("task_index", 0),
        ),
        "intermediate": {
            **state.get("intermediate", {}),
            "pending_generation": pending_generation,
        },
    }
