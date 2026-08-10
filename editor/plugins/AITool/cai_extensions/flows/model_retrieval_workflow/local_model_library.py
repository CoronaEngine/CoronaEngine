"""本地模型库：生成规划后按物体名优先查库，命中则复用、跳过混元3D 生成。

设计要点（详见 docs/实施修改记录.md 任务 E）：
- 全局按名复用：键 = item_name 归一化（strip().lower()，与 generate_images._image_cache 同款）。
- 全自动：generate 成功 → save_model 入库；retrieve 顶部 → lookup_model 命中即跳过生成。
- 简单：纯文件库（index.json + models/ + images/），不引入向量库/embedding。
- 零阻断：所有函数 try/except 包裹，库故障一律降级为"未命中→照常生成"，绝不中断主链路。

库结构（<project>/Resource/local_model_library/）：
    index.json                 {归一化名: {model_dir, image_file, created_at}}
    models/<safe>_<hash>/       original/ 原始模型目录 + runtime/ 轻量运行时目录
    images/<safe>_<hash>.png    复制进来的文生图图片

Canonical storage is ``<project>/Resource/local_model_library``.  The former
``assets/local_model_library`` location remains a read-only compatibility
fallback.
"""

from __future__ import annotations

import hashlib
import json
import logging
import os
import re
import shutil
import threading
import time
from typing import Optional

from .model_library_paths import resolve_model_library_root

logger = logging.getLogger(__name__)

_LIB_LOCK = threading.RLock()
_LIB_DIRNAME = "local_model_library"
_INDEX_NAME = "index.json"
_MODELS_SUBDIR = "models"
_IMAGES_SUBDIR = "images"


def _normalize_key(item_name: str) -> str:
    """归一化缓存键：去空白 + 小写（与 generate_images._normalize_cache_key 一致）。"""
    return (item_name or "").strip().lower()


def _safe_dirname(key: str) -> str:
    """归一化键 → 文件系统安全的目录/文件名（带短 hash 后缀防不同键碰撞）。"""
    cleaned = re.sub(r"\s+", "_", (key or "").strip())
    cleaned = re.sub(r"[^0-9A-Za-z_\-一-鿿]", "_", cleaned)
    cleaned = cleaned.strip("_") or "model"
    h = hashlib.sha1((key or "").encode("utf-8")).hexdigest()[:8]
    return f"{cleaned[:48]}_{h}"


def _now() -> str:
    """确定性时间戳（time.strftime，非 random，resume 安全）。"""
    return time.strftime("%Y-%m-%d %H:%M:%S")


def _lib_root(*, for_write: bool = False) -> Optional[str]:
    """库根目录绝对路径；项目路径取不到时返回 None（库降级为不可用）。"""
    try:
        from runtime.project_context import get_project_root

        project = get_project_root(fallback_to_environment=False)
        if not project:
            return None
        root = str(resolve_model_library_root(project, for_write=for_write))
        os.makedirs(root, exist_ok=True)
        return root
    except Exception as e:  # noqa: BLE001
        logger.warning("[LocalModelLib] 库根解析失败（降级不可用）: %s", e)
        return None


def _index_path(root: str) -> str:
    return os.path.join(root, _INDEX_NAME)


def _load_index(root: str) -> dict:
    p = _index_path(root)
    if not os.path.isfile(p):
        return {}
    try:
        with open(p, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except Exception as e:  # noqa: BLE001
        logger.warning("[LocalModelLib] index.json 读取失败（视为空库）: %s", e)
        return {}


def _save_index(root: str, index: dict) -> None:
    p = _index_path(root)
    try:
        tmp = p + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(index, f, ensure_ascii=False, indent=2)
        os.replace(tmp, p)  # 原子替换，避免半写坏 index
    except Exception as e:  # noqa: BLE001
        logger.warning("[LocalModelLib] index.json 写入失败: %s", e)


def _abs(root: str, rel_or_abs: str) -> str:
    """库内相对路径 → 绝对路径（已是绝对则原样返回）。"""
    if not rel_or_abs:
        return ""
    return rel_or_abs if os.path.isabs(rel_or_abs) else os.path.join(root, rel_or_abs)


# ---------------------------------------------------------------------------
# 模型库（主路径，省混元3D token）
# ---------------------------------------------------------------------------

def lookup_model(item_name: str) -> Optional[str]:
    """按名查模型库。命中返回库内模型目录绝对路径（经 resolve_model_file 校验有真模型文件），
    否则返回 None；条目失效（文件缺失）则清理脏条目。"""
    try:
        key = _normalize_key(item_name)
        if not key:
            return None
        with _LIB_LOCK:
            root = _lib_root()
            if not root:
                return None
            index = _load_index(root)
            entry = index.get(key)
            if not entry:
                return None
            from .helpers import resolve_model_file

            candidates = []
            for key_name in ("runtime_model_dir", "model_dir", "original_model_dir"):
                value = entry.get(key_name, "")
                if value and value not in candidates:
                    candidates.append(value)
            for model_dir in candidates:
                abs_dir = _abs(root, model_dir)
                resolved = resolve_model_file(abs_dir)
                if resolved and os.path.exists(resolved):
                    return abs_dir  # 返回目录，下游 collect_models 会再 resolve 取首个模型文件
            # 脏条目：模型文件已不存在 → 清理，视为未命中
            logger.info("[LocalModelLib] %s 库条目失效（文件缺失），清理: %s", item_name, entry)
            index.pop(key, None)
            _save_index(root, index)
            return None
    except Exception as e:  # noqa: BLE001
        logger.warning("[LocalModelLib] lookup_model 异常（降级未命中）: %s", e)
        return None


def save_model(item_name: str, model_path: str) -> None:
    """生成成功后自动入库（幂等）：解析真实模型文件 → 复制其父目录到 models/，写 index。
    已有有效条目则跳过。失败仅 warning，绝不影响主链路。"""
    try:
        key = _normalize_key(item_name)
        if not key or not model_path:
            return
        with _LIB_LOCK:
            root = _lib_root(for_write=True)
            if not root:
                return

            from .helpers import resolve_model_file

            index = _load_index(root)
            # 幂等：已有 runtime 有效条目则跳过；旧 model_dir-only 条目允许本次升级。
            existing_entry = index.get(key, {})
            existing_runtime = (
                existing_entry.get("runtime_model_dir")
                or (
                    existing_entry.get("model_dir", "")
                    if "/runtime" in existing_entry.get("model_dir", "").replace("\\", "/")
                    else ""
                )
            )
            if existing_runtime and resolve_model_file(_abs(root, existing_runtime)):
                return

            resolved = resolve_model_file(model_path)
            if not resolved or not os.path.exists(resolved):
                logger.warning("[LocalModelLib] %s 存库跳过：模型文件解析失败 %s", item_name, model_path)
                return
            original_model = _resolve_original_model_path(resolved)
            src_dir = os.path.dirname(original_model)
            if not os.path.isdir(src_dir):
                return

            dirname = _safe_dirname(key)
            models_root = os.path.join(root, _MODELS_SUBDIR)
            os.makedirs(models_root, exist_ok=True)
            dst_dir = os.path.join(models_root, dirname)
            if os.path.isdir(dst_dir):
                shutil.rmtree(dst_dir, ignore_errors=True)
            original_dir = os.path.join(dst_dir, "original")
            shutil.copytree(
                src_dir,
                original_dir,
                ignore=shutil.ignore_patterns("runtime", "original"),
            )
            copied_original_model = os.path.join(original_dir, os.path.basename(original_model))

            from .runtime_assets import prepare_runtime_model_bundle

            runtime_bundle = prepare_runtime_model_bundle(copied_original_model)
            runtime_dir = os.path.dirname(runtime_bundle.runtime_model_path)

            entry = index.get(key, {})
            entry["original_model_dir"] = os.path.join(_MODELS_SUBDIR, dirname, "original")
            entry["runtime_model_dir"] = os.path.relpath(runtime_dir, root)
            entry["runtime_texture_max"] = 1024
            entry["model_dir"] = entry["runtime_model_dir"]
            entry["created_at"] = _now()
            index[key] = entry
            _save_index(root, index)
            logger.info("[LocalModelLib] %s 已存入本地模型库: %s", item_name, runtime_dir)
    except Exception as e:  # noqa: BLE001
        logger.warning("[LocalModelLib] save_model 异常（不影响主链路）: %s", e)


def _resolve_original_model_path(model_path: str) -> str:
    path = os.path.abspath(model_path)
    parent = os.path.basename(os.path.dirname(path))
    if parent != "runtime":
        return path
    model_name = os.path.basename(path)
    root_dir = os.path.dirname(os.path.dirname(path))
    direct_sibling = os.path.join(root_dir, model_name)
    if os.path.isfile(direct_sibling):
        return direct_sibling
    original_sibling = os.path.join(root_dir, "original", model_name)
    if os.path.isfile(original_sibling):
        return original_sibling
    return path


# ---------------------------------------------------------------------------
# 图片库（次路径，"尽量"省文生图 token）
# ---------------------------------------------------------------------------

def lookup_image(item_name: str) -> Optional[str]:
    """按名查图片库。命中返回本地图片绝对路径（文件存在才算命中），否则 None。"""
    try:
        key = _normalize_key(item_name)
        if not key:
            return None
        with _LIB_LOCK:
            root = _lib_root()
            if not root:
                return None
            entry = _load_index(root).get(key) or {}
            img = entry.get("image_file", "")
            if not img:
                return None
            abs_img = _abs(root, img)
            return abs_img if os.path.isfile(abs_img) else None
    except Exception as e:  # noqa: BLE001
        logger.warning("[LocalModelLib] lookup_image 异常（降级未命中）: %s", e)
        return None


def _fetch_to(src: str, dst: str) -> bool:
    """把图片源（http(s)/file:///fileid:///本地路径）取到 dst。成功返回 True。"""
    try:
        s = str(src or "").strip()
        if not s:
            return False
        if s.startswith("fileid://"):
            try:
                from Quasar.ai_media_resource import get_media_registry

                resolved = str(get_media_registry().resolve(s[len("fileid://"):]) or "").strip()
                if resolved:
                    s = resolved
            except Exception:  # noqa: BLE001
                pass
        if s.startswith(("http://", "https://")):
            import urllib.request

            req = urllib.request.Request(s, headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req, timeout=30) as resp:  # noqa: S310
                data = resp.read()
            with open(dst, "wb") as f:
                f.write(data)
            return True
        if s.startswith("file://"):
            s = s[len("file://"):]
        if os.path.isfile(s):
            shutil.copyfile(s, dst)
            return True
        logger.warning("[LocalModelLib] 图片源无法识别/不存在: %s", src)
        return False
    except Exception as e:  # noqa: BLE001
        logger.warning("[LocalModelLib] 图片抓取失败 %s: %s", src, e)
        return False


def save_image(item_name: str, src_url_or_path: str) -> None:
    """文生图成功后落盘缓存（best-effort）。失败仅 warning，绝不影响主链路。"""
    try:
        key = _normalize_key(item_name)
        if not key or not src_url_or_path:
            return
        with _LIB_LOCK:
            root = _lib_root(for_write=True)
            if not root:
                return
            images_root = os.path.join(root, _IMAGES_SUBDIR)
            os.makedirs(images_root, exist_ok=True)
            rel = os.path.join(_IMAGES_SUBDIR, _safe_dirname(key) + ".png")
            dst = os.path.join(root, rel)
            if not _fetch_to(src_url_or_path, dst):
                return
            index = _load_index(root)
            entry = index.get(key, {})
            entry["image_file"] = rel
            index[key] = entry
            _save_index(root, index)
            logger.info("[LocalModelLib] %s 图片已缓存: %s", item_name, dst)
    except Exception as e:  # noqa: BLE001
        logger.warning("[LocalModelLib] save_image 异常（不影响主链路）: %s", e)
