"""ModelProvider — Agent 的 3D 模型获取入口。

把「搜索本地模型库 → 文字生成3D → 下载模型文件」串成一条链，
返回可导入引擎的本地模型文件路径。

用法:
    provider = ModelProvider()
    model_info = provider.acquire("台灯", image_url="...")  # 阻塞

全程 Verbose 日志，方便验收追踪。
"""

from __future__ import annotations

import json
import logging
import os
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

logger = logging.getLogger(__name__)

# ═══════════════════════════════════════════════════════════════════════════
# helpers
# ═══════════════════════════════════════════════════════════════════════════

_SUPPORTED_EXTENSIONS = {".obj", ".dae", ".glb", ".gltf", ".fbx", ".stl", ".usdz"}


def _resolve_model_file(path: str) -> str:
    """从目录或文件路径中提取第一个支持的 3D 模型文件。"""
    path_text = str(path or "").strip()
    if not path_text:
        return ""
    p = Path(path_text)
    if p.is_file() and p.suffix.lower() in _SUPPORTED_EXTENSIONS:
        return str(p)
    if p.is_dir():
        for entry in sorted(p.iterdir()):
            if entry.is_file() and entry.suffix.lower() in _SUPPORTED_EXTENSIONS:
                return str(entry)
    return ""


class AcquireResult:
    """模型获取结果。"""

    def __init__(self, success: bool, local_path: str = "", source: str = "",
                 preview_images: List[str] = None, error: str = "",
                 metadata: Dict[str, Any] = None) -> None:
        self.success = success
        self.local_path = local_path          # 可导入引擎的 .glb/.fbx 路径
        self.source = source                   # "search" | "generation" | "fallback"
        self.preview_images = preview_images or []
        self.error = error
        self.metadata = metadata or {}

    def __repr__(self) -> str:
        return (f"AcquireResult(success={self.success}, source={self.source!r}, "
                f"path={self.local_path!r}, error={self.error!r})")


# ═══════════════════════════════════════════════════════════════════════════
# ModelProvider
# ═══════════════════════════════════════════════════════════════════════════

class ModelProvider:
    """Agent 侧 3D 模型获取器。

    流程:  搜索本地模型库 → 命中 → 返回本地路径
                          → 未命中 → 文字生成3D → 下载 → 返回本地路径
    """

    def __init__(self, config: Dict[str, Any] = None) -> None:
        self._cfg = config or {}
        self._models_dir: Optional[str] = None
        self._download_root: Optional[str] = None

    # ── 主入口 ────────────────────────────────────────────────────────

    def acquire(self, name: str, image_url: str = "",
                prompt_text: str = "", object_id: str = "") -> AcquireResult:
        """为 Agent 获取一个物体的 3D 模型文件。

        Args:
            name: 物体名称 (如 "台灯", "双人床")
            image_url: 参考图 URL（可选，有图优先走 image_to_3d）
            prompt_text: 文字描述（用于生成 prompt）
            object_id: 可选 object_id（用于目录命名）

        Returns:
            AcquireResult
        """
        t0 = time.perf_counter()
        oid = object_id or name
        logger.info("[ModelProvider] === acquire START: name=%r, image=%r ===",
                    name, image_url[:80] if image_url else "(none)")

        # 步骤 1: 搜索本地模型库
        search_result = self._search_library(name, image_url)
        if search_result.success:
            elapsed = time.perf_counter() - t0
            logger.info("[ModelProvider] === acquire DONE (search hit) name=%r "
                        "path=%r elapsed=%.2fs ===", name, search_result.local_path, elapsed)
            return search_result

        logger.info("[ModelProvider] search miss for %r, proceeding to 3D generation...", name)

        # 步骤 2: 文字/图片生成 3D
        gen_result = self._generate_3d(name, image_url=image_url,
                                        prompt_text=prompt_text, object_id=oid)
        if gen_result.success:
            elapsed = time.perf_counter() - t0
            logger.info("[ModelProvider] === acquire DONE (generation) name=%r "
                        "path=%r elapsed=%.2fs ===", name, gen_result.local_path, elapsed)
            return gen_result

        elapsed = time.perf_counter() - t0
        logger.error("[ModelProvider] === acquire FAILED name=%r elapsed=%.2fs "
                     "error=%r ===", name, elapsed, gen_result.error)
        return AcquireResult(success=False, source="none",
                             error=f"搜索未命中且 3D 生成失败: {gen_result.error}")

    # ── 步骤 1: 搜索 ──────────────────────────────────────────────────

    def _search_library(self, name: str, image_url: str = "") -> AcquireResult:
        """在本地模型向量库中搜索匹配的 3D 模型。"""
        logger.info("[ModelProvider][search] searching name=%r...", name)
        started = time.perf_counter()

        search_tool = self._get_tool("search_similar_object")
        if not search_tool:
            logger.warning("[ModelProvider][search] search_similar_object 工具不可用")
            return AcquireResult(success=False, source="search",
                                 error="搜索工具不可用")

        try:
            invoke_args: Dict[str, Any] = {"query_text": name, "top_k": 1}
            if image_url:
                invoke_args["query_images"] = [image_url]

            raw = search_tool.invoke(invoke_args)
            parsed = self._parse_search(raw)

            elapsed = time.perf_counter() - started
            if parsed.get("hit") and parsed.get("best_match"):
                best = parsed["best_match"]
                model_path_raw = best.get("model_path", "")
                model_path = _resolve_model_file(model_path_raw) if model_path_raw else ""
                distance = best.get("distance", 0)

                logger.info("[ModelProvider][search] HIT name=%r object_id=%r "
                            "distance=%.4f path=%r elapsed=%.2fs",
                            name, best.get("object_id", ""), distance, model_path, elapsed)

                if model_path and os.path.exists(model_path):
                    return AcquireResult(success=True, source="search",
                                         local_path=model_path,
                                         preview_images=best.get("image_paths", []),
                                         metadata={"distance": distance,
                                                   "object_id": best.get("object_id", "")})
                else:
                    logger.warning("[ModelProvider][search] model_path 无效: %r", model_path_raw)

            logger.info("[ModelProvider][search] MISS name=%r elapsed=%.2fs", name, elapsed)
            return AcquireResult(success=False, source="search",
                                 error="本地模型库未找到匹配项")

        except Exception as e:
            elapsed = time.perf_counter() - started
            logger.warning("[ModelProvider][search] exception: %s (elapsed=%.2fs)", e, elapsed)
            return AcquireResult(success=False, source="search", error=str(e))

    # ── 步骤 2: 生成 ──────────────────────────────────────────────────

    def _generate_3d(self, name: str, image_url: str = "",
                     prompt_text: str = "", object_id: str = "") -> AcquireResult:
        """调用 3D 生成工具 (Hunyuan3D / Rodin) 并等待模型下载完成。"""
        tool = self._get_3d_generate_tool()
        if not tool:
            logger.warning("[ModelProvider][generate] 3D 生成工具不可用")
            return AcquireResult(success=False, source="generation",
                                 error="3D 生成工具 (Hunyuan3D/Rodin) 不可用")

        # 选择生成模式
        if image_url and image_url.startswith("__text_to_3d__:"):
            mode = "text_to_3d"
            prompt = image_url[len("__text_to_3d__:"):] or prompt_text or name
            tool_input = {"mode": mode, "prompt": prompt, "object_id": object_id}
            logger.info("[ModelProvider][generate] mode=text_to_3d prompt=%r...",
                        prompt[:80])
        elif image_url:
            mode = "image_to_3d"
            tool_input = {"mode": mode, "images": [image_url],
                          "object_id": object_id, "prompt": name}
            logger.info("[ModelProvider][generate] mode=image_to_3d image=%r...",
                        image_url[:80])
        else:
            mode = "text_to_3d"
            prompt = prompt_text or f"high quality 3D model of {name}"
            tool_input = {"mode": mode, "prompt": prompt, "object_id": object_id}
            logger.info("[ModelProvider][generate] mode=text_to_3d (no image) "
                        "prompt=%r...", prompt[:80])

        started = time.perf_counter()
        logger.info("[ModelProvider][generate] invoking %s for %r...", mode, name)

        try:
            raw = tool.invoke(tool_input)
            model_info = self._parse_3d_result(raw)

            if model_info.get("error"):
                elapsed = time.perf_counter() - started
                logger.error("[ModelProvider][generate] FAILED: %s (elapsed=%.2fs)",
                             model_info["error"], elapsed)
                return AcquireResult(success=False, source="generation",
                                     error=model_info["error"])

            # 等待后台 mesh 下载完成
            param = model_info.get("parameter", {})
            if param.get("has_mesh_pending"):
                logger.info("[ModelProvider][generate] waiting for mesh download...")
                self._wait_for_mesh(object_id)

            # 解析最终模型文件路径
            raw_path = model_info.get("model_path", "")
            local_path = _resolve_model_file(raw_path)
            elapsed = time.perf_counter() - started

            if local_path and os.path.exists(local_path):
                size_kb = os.path.getsize(local_path) / 1024
                logger.info("[ModelProvider][generate] SUCCESS name=%r path=%r "
                            "size=%.1fKB elapsed=%.2fs", name, local_path, size_kb, elapsed)
                return AcquireResult(success=True, source="generation",
                                     local_path=local_path,
                                     preview_images=param.get("preview_paths", []),
                                     metadata={"model_folder": param.get("model_folder", ""),
                                               "format": param.get("geometry_file_format", "glb")})
            else:
                logger.warning("[ModelProvider][generate] 模型文件未找到: raw=%r resolved=%r",
                               raw_path, local_path)
                return AcquireResult(success=False, source="generation",
                                     error=f"模型文件下载后未找到: {raw_path}")

        except Exception as e:
            elapsed = time.perf_counter() - started
            logger.exception("[ModelProvider][generate] exception: %s (elapsed=%.2fs)",
                             e, elapsed)
            return AcquireResult(success=False, source="generation", error=str(e))

    # ── 工具获取 ──────────────────────────────────────────────────────

    def _get_tool(self, name: str) -> Any:
        try:
            from Quasar.ai_tools.registry import get_tool_registry
            registry = get_tool_registry()
            tools = registry.list_tools()
            if not tools:
                from Quasar.ai_tools.load_tools import load_tools
                from Quasar.ai_config.ai_config import get_ai_config
                load_tools(get_ai_config())
                tools = registry.list_tools()
            return {t.name: t for t in tools}.get(name)
        except Exception as e:
            logger.warning("[ModelProvider] get_tool(%s) failed: %s", name, e)
            return None

    def _get_3d_generate_tool(self) -> Any:
        try:
            from Quasar.ai_config.ai_config import get_ai_config
            config = get_ai_config()
            hunyuan_cfg = getattr(config, 'hunyuan3d', None)
            if hunyuan_cfg is not None and getattr(hunyuan_cfg, 'enable', False):
                tool = self._get_tool("hunyuan_generate_3d")
                if tool:
                    return tool
            logger.warning("混元 3D 服务不可用，本次模型生成无法继续；Rodin fallback 已关闭")
            return None
        except Exception as e:
            logger.warning("混元 3D 配置读取失败，本次模型生成无法继续；Rodin fallback 已关闭: %s", e)
            return None

    # ── 解析 ──────────────────────────────────────────────────────────

    def _parse_search(self, raw: Any) -> Dict[str, Any]:
        if isinstance(raw, dict):
            parsed = raw
        elif isinstance(raw, str):
            parsed = json.loads(raw)
        else:
            return {"hit": False}

        error_code = parsed.get("error_code", 0)
        if error_code:
            return {"hit": False, "error": parsed.get("status_info", "")}

        param = {}
        try:
            llm = parsed.get("llm_content", [{}])
            parts = llm[0].get("part", [{}]) if llm else [{}]
            param = parts[0].get("parameter", {})
        except (IndexError, KeyError, TypeError):
            pass

        hit = bool(param.get("hit", False))
        best_match = param.get("best_match") or parsed.get("best_match")
        if not isinstance(best_match, dict):
            matches = param.get("matches", [])
            best_match = matches[0] if matches else None

        return {"hit": hit, "best_match": best_match}

    def _parse_3d_result(self, raw: Any) -> Dict[str, Any]:
        if isinstance(raw, dict):
            parsed = raw
        elif isinstance(raw, str):
            parsed = json.loads(raw)
        else:
            return {"error": "不支持的工具返回类型"}

        error_code = parsed.get("error_code", 0)
        if error_code:
            return {"error": parsed.get("status_info", "")}

        metadata = parsed.get("metadata", {})
        try:
            llm = parsed.get("llm_content", [{}])
            parts = llm[0].get("part", []) if llm else []
        except (IndexError, KeyError, TypeError):
            parts = []

        model_folder = metadata.get("model_folder", "")
        preview_paths: List[str] = []
        geometry_file_format = "glb"

        for part in parts:
            if part.get("content_type") == "image":
                url = part.get("content_url", "") or part.get("content_text", "")
                if url:
                    preview_paths.append(url)
                fmt = (part.get("parameter") or {}).get("geometry_file_format", "")
                if fmt:
                    geometry_file_format = fmt

        geo_fmt = metadata.get("geometry_file_format", geometry_file_format)
        has_mesh_pending = bool(metadata.get("has_mesh_pending", False))

        return {
            "model_path": model_folder,
            "parameter": {
                "preview_paths": preview_paths,
                "model_folder": model_folder,
                "geometry_file_format": geo_fmt,
                "has_mesh_pending": has_mesh_pending,
                "object_id": metadata.get("folder_object_id", ""),
            },
        }

    def _wait_for_mesh(self, object_id: str) -> None:
        try:
            from Quasar.ai_modules.three_d_generate.tools.model_tools import wait_for_mesh_ready
            logger.info("[ModelProvider][mesh] waiting for %r...", object_id)
            wait_for_mesh_ready(object_id)
            logger.info("[ModelProvider][mesh] %r download complete", object_id)
        except ImportError:
            time.sleep(1.0)  # fallback


__all__ = ["ModelProvider", "AcquireResult"]
