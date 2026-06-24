from __future__ import annotations

import json
import logging
import os
import re
import time
from pathlib import Path
from typing import Any, Dict, List

import numpy as np

from Quasar.ai_config.ai_config import get_ai_config
from Quasar.ai_tools.registry import get_tool_registry
from Quasar.ai_tools.response_adapter import FILEID_SCHEME
from Quasar.ai_config.paths_config import _get_active_project_path

logger = logging.getLogger(__name__)

PREVIEW_IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".webp"}
OBJECT_SEARCH_KEY_ENV_NAMES = ("DASHSCOPE_API_KEY", "DASH_SCOPE_API_KEY")
OBJECT_EMBEDDING_ENABLE_ENV = "CORONA_ENABLE_OBJECT_EMBEDDING"


def _resolve_preview_part_url(part: Dict[str, Any]) -> str:
    """解析 3D 工具 image part，优先返回可展示的预览图路径。"""
    raw_url = str(part.get("content_url") or "").strip()
    if raw_url.startswith(FILEID_SCHEME):
        file_id = raw_url[len(FILEID_SCHEME):].strip()
        if file_id:
            try:
                from Quasar.ai_media_resource import get_media_registry
                resolved = str(get_media_registry().resolve(file_id) or "").strip()
                if resolved:
                    return resolved
            except Exception as exc:  # noqa: BLE001
                logger.warning("3D 预览图 file_id 解析失败: %s, err=%s", file_id, exc)

    for key in ("content_url", "content_path", "content_text"):
        candidate = str(part.get(key) or "").strip()
        if not candidate:
            continue

        lowered = candidate.lower()
        if lowered.startswith(("http://", "https://", "data:", "file://")):
            return candidate

        path_obj = Path(candidate)
        if path_obj.is_absolute():
            return str(path_obj)

        suffix = path_obj.suffix.lower()
        if suffix in PREVIEW_IMAGE_EXTENSIONS:
            abs_path = (_get_active_project_path() / path_obj).resolve()
            return str(abs_path)

    return ""


def normalize_object_id(name: str, fallback_index: int) -> str:
    """将物体名转换为 object_id 友好的目录名。"""
    cleaned = re.sub(r"\s+", "_", (name or "").strip())
    cleaned = re.sub(r"[^0-9A-Za-z_\-\u4e00-\u9fff]", "_", cleaned)
    cleaned = cleaned.strip("_")
    if not cleaned:
        cleaned = f"object_{fallback_index:02d}"
    return cleaned[:64]


def get_tool(name: str) -> Any:
    """从工具注册表中按名称获取工具，按需触发懒加载。"""
    registry = get_tool_registry()
    tools = registry.list_tools()
    if not tools:
        from Quasar.ai_tools.load_tools import load_tools

        load_tools(get_ai_config())
        tools = registry.list_tools()
    return {t.name: t for t in tools}.get(name)


def _read_config_value(raw: Any, key: str, default: Any = None) -> Any:
    if isinstance(raw, dict):
        return raw.get(key, default)
    return getattr(raw, key, default)


def _provider_api_key(providers: Any, provider_name: str) -> str:
    if not provider_name:
        return ""

    provider = None
    try:
        if isinstance(providers, dict):
            provider = providers.get(provider_name)
        elif hasattr(providers, "get"):
            provider = providers.get(provider_name)
    except Exception as exc:  # noqa: BLE001
        logger.debug("object search provider lookup failed: %s", exc)

    if provider is None:
        return ""
    return str(_read_config_value(provider, "api_key", "") or "").strip()


def _as_bool(value: Any, default: bool = False) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    if isinstance(value, (int, float)):
        return bool(value)
    if isinstance(value, str):
        normalized = value.strip().lower()
        if normalized in {"1", "true", "yes", "on", "enable", "enabled"}:
            return True
        if normalized in {"0", "false", "no", "off", "disable", "disabled"}:
            return False
    return default


def object_embedding_tools_enabled(config: Any | None = None) -> bool:
    """Dashscope embedding/search is disabled for the formal validation profile."""
    return False


def _object_search_has_embedding_key(config: Any) -> bool:
    """判断图搜嵌入服务是否具备可用凭据，避免运行期批量 401。"""
    if not object_embedding_tools_enabled(config):
        return False

    recognition_cfg = _read_config_value(config, "object_recognition", {}) or {}
    enabled = _read_config_value(recognition_cfg, "enable", True)
    if enabled is False:
        return False

    provider_name = str(_read_config_value(recognition_cfg, "provider", "dashscope") or "dashscope").strip()
    direct_key = str(_read_config_value(recognition_cfg, "dashscope_api_key", "") or "").strip()
    provider_key = _provider_api_key(_read_config_value(config, "providers", {}) or {}, provider_name)
    env_key = next((os.environ.get(name, "").strip() for name in OBJECT_SEARCH_KEY_ENV_NAMES if os.environ.get(name, "").strip()), "")

    return bool(direct_key or provider_key or env_key)


def get_search_tool():
    """获取物体搜索工具。"""
    logger.info("正式试验配置已关闭图搜/embedding，跳过 search_similar_object")
    return None


def get_store_tool():
    """获取物体入库工具。"""
    logger.info("正式试验配置已关闭图搜/embedding，跳过 store_object")
    return None


def get_3d_generate_tool():
    """获取 3D 模型生成工具。正式试验只使用混元3D，不回退 Rodin。"""
    from Quasar.ai_config.ai_config import get_ai_config
    config = get_ai_config()
    hunyuan_cfg = getattr(config, 'hunyuan3d', None)
    if hunyuan_cfg is not None and getattr(hunyuan_cfg, 'enable', False):
        tool = get_tool("hunyuan_generate_3d")
        if tool is not None:
            return tool
    logger.warning("混元 3D 服务不可用，本次模型生成无法继续；Rodin fallback 已关闭")
    return None


def parse_tool_result(raw_result: Any) -> Dict[str, Any]:
    """解析工具 envelope，统一返回字典结构。"""
    if isinstance(raw_result, dict):
        return raw_result
    if isinstance(raw_result, str):
        return json.loads(raw_result)
    raise TypeError(f"不支持的工具返回类型: {type(raw_result)!r}")


def extract_tool_error(parsed_result: Dict[str, Any]) -> str:
    """从工具 envelope 中提取错误信息。"""
    error_code = parsed_result.get("error_code", 0)
    if not error_code:
        return ""

    status_info = str(parsed_result.get("status_info", "") or "").strip()
    if status_info and status_info.lower() != "success":
        return status_info

    try:
        parts = parsed_result["llm_content"][0]["part"]
        for part in parts:
            text = str(part.get("content_text", "") or "").strip()
            if text:
                return text
    except (KeyError, IndexError, TypeError):
        pass

    return "工具调用失败"


def extract_tool_parts(parsed_result: Dict[str, Any]) -> List[Dict[str, Any]]:
    """从工具 envelope 中提取 part 列表。"""
    try:
        parts = parsed_result["llm_content"][0]["part"]
    except (KeyError, IndexError, TypeError):
        return []
    return parts if isinstance(parts, list) else []


def extract_first_part_parameter(parsed_result: Dict[str, Any]) -> Dict[str, Any]:
    """提取第一条 part.parameter。"""
    for part in extract_tool_parts(parsed_result):
        parameter = part.get("parameter")
        if isinstance(parameter, dict):
            return parameter
    return {}


def parse_search_result(raw_result: Any) -> Dict[str, Any]:
    """解析物体搜索工具结果，兼容 dict 与 JSON 字符串。"""
    parsed = parse_tool_result(raw_result)
    error_message = extract_tool_error(parsed)
    if error_message:
        return {"error": error_message}

    part_parameter = extract_first_part_parameter(parsed)

    hit = parsed.get("hit")
    if not isinstance(hit, bool):
        hit = bool(part_parameter.get("hit", False))

    all_matches = parsed.get("all_matches")
    if not isinstance(all_matches, list):
        fallback_matches = part_parameter.get("matches", [])
        all_matches = fallback_matches if isinstance(fallback_matches, list) else []

    best_match = parsed.get("best_match")
    if not isinstance(best_match, dict):
        first_match = all_matches[0] if all_matches else None
        best_match = first_match if isinstance(first_match, dict) else None

    return {
        "hit": hit,
        "best_match": best_match,
        "all_matches": all_matches,
    }


def parse_store_result(raw_result: Any) -> Dict[str, Any]:
    """解析物体入库工具结果，兼容 dict 与 JSON 字符串。"""
    parsed = parse_tool_result(raw_result)
    error_message = extract_tool_error(parsed)
    if error_message:
        return {"error": error_message}

    part_parameter = extract_first_part_parameter(parsed)
    register_status = str(
        parsed.get("register_status")
        or part_parameter.get("register_status")
        or "inserted"
    ).strip() or "inserted"

    return {
        "register_status": register_status,
        "rowid": parsed.get("rowid", part_parameter.get("rowid")),
        "object_id": parsed.get("object_id", part_parameter.get("object_id", "")),
    }


def parse_3d_result(raw_result: Any) -> Dict[str, Any]:
    """解析 rodin_generate_3d 返回值，提取模型文件路径与元数据。"""
    try:
        parsed = parse_tool_result(raw_result)
        error_message = extract_tool_error(parsed)
        if error_message:
            return {"error": error_message}

        metadata = parsed.get("metadata") or {}
        model_folder: str = metadata.get("model_folder", "")
        has_mesh_pending: bool = metadata.get("has_mesh_pending", False)
        folder_object_id: str = metadata.get("folder_object_id", "") or metadata.get(
            "object_id", ""
        )
        mesh_object_id: str = metadata.get("mesh_object_id", "") or metadata.get(
            "model_object_id", ""
        )

        parts = parsed["llm_content"][0]["part"]
        preview_paths: List[str] = []
        geometry_file_format = "glb"

        for part in parts:
            if part.get("content_type") == "image":
                preview_path = _resolve_preview_part_url(part)
                if preview_path:
                    preview_paths.append(preview_path)
                part_param = part.get("parameter") or {}
                fmt = part_param.get("geometry_file_format", "")
                if fmt:
                    geometry_file_format = fmt

        geometry_file_format = metadata.get(
            "geometry_file_format",
            geometry_file_format,
        )

        if model_folder:
            model_path = model_folder  # 目录路径，resolve_model_file 会扫描目录内文件
            parameter: Dict[str, Any] = {
                "preview_paths": preview_paths,
                "model_folder": model_folder,
                "geometry_file_format": geometry_file_format,
                "has_mesh_pending": has_mesh_pending,
                "object_id": folder_object_id,
                "folder_object_id": folder_object_id,
                "mesh_object_id": mesh_object_id,
            }
            return {
                "model_path": model_path,
                "parameter": parameter,
            }
    except (json.JSONDecodeError, KeyError, IndexError, TypeError, ValueError):
        pass
    return {"error": "3D 生成结果解析失败"}


def pick_first_preview_path(*candidates: Any) -> str:
    """从若干候选图片路径集合中选择第一条可用路径。"""
    for candidate in candidates:
        if isinstance(candidate, str):
            text = candidate.strip()
            if text:
                return text
            continue

        if isinstance(candidate, list):
            for item in candidate:
                text = str(item or "").strip()
                if text:
                    return text
    return ""


def find_sibling_preview_image(model_path: str) -> str:
    """在模型同目录中查找第一张预览图（不递归）。"""
    path_text = str(model_path or "").strip()
    if not path_text:
        return ""

    lowered = path_text.lower()
    if lowered.startswith("http://") or lowered.startswith("https://"):
        return ""

    model_file = Path(path_text)
    model_dir = model_file if model_file.is_dir() else model_file.parent
    if not model_dir.exists() or not model_dir.is_dir():
        return ""

    try:
        for entry in sorted(model_dir.iterdir(), key=lambda item: item.name.lower()):
            if not entry.is_file():
                continue
            if entry.suffix.lower() in PREVIEW_IMAGE_EXTENSIONS:
                return str(entry)
    except OSError:
        return ""

    return ""


def build_placeholder_embedding(
    object_id: str,
    model_path: str,
    vector_dim: int,
) -> np.ndarray:
    """生成可复现的伪向量兜底，仅在嵌入模型不可用时使用。"""
    import hashlib

    seed_text = f"{object_id}|{model_path}"
    seed_bytes = hashlib.sha256(seed_text.encode("utf-8")).digest()[:8]
    seed = int.from_bytes(seed_bytes, byteorder="big", signed=False)
    rng = np.random.default_rng(seed)
    vec = rng.standard_normal(vector_dim).astype(np.float32)
    norm = np.linalg.norm(vec)
    if norm > 1e-12:
        vec = vec / norm
    return vec


def resolve_model_file(model_path: str) -> str:
    """解析模型路径并返回可用的 3D 模型文件路径。"""
    path_text = str(model_path or "").strip()
    if not path_text:
        return ""

    if os.path.isabs(path_text):
        resolved_path = path_text
    else:
        project_path = str(_get_active_project_path())
        resolved_path = os.path.join(project_path, path_text) if project_path else path_text

    supported_exts = {".obj", ".dae", ".glb", ".gltf", ".fbx", ".stl", ".usdz"}

    def _scan_dir(dir_path: str) -> str:
        """扫描目录，返回第一个支持的模型文件。"""
        if not os.path.isdir(dir_path):
            return ""
        runtime_dir = os.path.join(dir_path, "runtime")
        if os.path.isdir(runtime_dir):
            runtime_result = _scan_dir(runtime_dir)
            if runtime_result:
                return runtime_result
        for entry in sorted(os.listdir(dir_path)):
            _, ext = os.path.splitext(entry)
            if ext.lower() in supported_exts:
                return os.path.join(dir_path, entry)
        return ""

    if os.path.isfile(resolved_path):
        if any(resolved_path.lower().endswith(ext) for ext in supported_exts):
            return resolved_path
        return ""

    if os.path.isdir(resolved_path):
        return _scan_dir(resolved_path) or ""

    # 精确文件不存在，尝试去掉文件名部分，扫描父目录
    parent_dir = os.path.dirname(resolved_path)
    result = _scan_dir(parent_dir)
    if result:
        return result

    return ""


def wait_mesh_then_resolve_model_file(
    *,
    raw_model_path: str,
    wait_object_id: str,
    has_mesh_pending: bool,
    retry_times: int = 3,
    retry_interval_seconds: float = 0.2,
) -> str:
    """在使用模型前等待后台 mesh 下载完成，并重试解析模型文件路径。"""
    wait_for_pending_mesh(
        parameter={"has_mesh_pending": has_mesh_pending, "object_id": wait_object_id},
        fallback_object_id=wait_object_id,
        stage="capture",
        wait_reason="截图前门禁",
    )

    max_retry = max(1, int(retry_times))
    for attempt in range(max_retry):
        final_model_path = resolve_model_file(raw_model_path)
        if final_model_path and os.path.exists(final_model_path):
            return final_model_path

        if attempt < max_retry - 1:
            time.sleep(max(0.0, float(retry_interval_seconds)))

    return ""


def wait_for_pending_mesh(
    *,
    parameter: Dict[str, Any],
    fallback_object_id: str,
    stage: str,
    wait_reason: str = "",
) -> bool:
    """当 parameter 标记 has_mesh_pending 时，阻塞等待后台 mesh 下载完成。"""
    if not bool(parameter.get("has_mesh_pending", False)):
        return False

    wait_object_id = str(parameter.get("object_id") or fallback_object_id or "").strip()
    if not wait_object_id:
        return False

    from Quasar.ai_modules.three_d_generate.tools.model_tools import wait_for_mesh_ready

    reason_suffix = f"（{wait_reason}）" if wait_reason else ""
    logger.info(
        "[Workflow][%s] %s 等待后台 mesh 下载完成%s...",
        stage,
        wait_object_id,
        reason_suffix,
    )
    wait_for_mesh_ready(wait_object_id)
    logger.info("[Workflow][%s] %s mesh 下载已完成", stage, wait_object_id)
    return True
