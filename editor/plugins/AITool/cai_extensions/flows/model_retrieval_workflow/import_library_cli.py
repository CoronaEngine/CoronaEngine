"""把一个"物体名目录/"形式的模型文件夹批量导入本地模型库（任务 E 配套工具）。

用途：复用历史已生成的模型（如 hs_test/models），免得 F5 重新生成、烧混元 token。

与 local_model_library.py 完全对齐（必须一致，否则引擎 lookup_model 找不到）。模型库归档到项目 Resource：
- key = item_name.strip().lower()
- 库目录名 = _safe_dirname(key) = clean(key)[:48] + "_" + sha1(key)[:8]
- 库根 = <project>/Resource/local_model_library/，index.json + models/<safe>/

按基名去重：源目录 "地毯"/"地毯_1".."地毯_10" 是同一物体多次生成，
归并到物体名 "地毯"，每个物体只入一个代表版本（优先无后缀目录）。

用法：
    python import_library_cli.py --src <models_dir> --project <project_path> [--force]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import time

try:
    from .model_library_paths import canonical_model_library_root
except ImportError:  # direct ``python import_library_cli.py`` compatibility
    from model_library_paths import canonical_model_library_root

_MODEL_EXTS = {".obj", ".dae", ".glb", ".gltf", ".fbx", ".stl", ".usdz"}
_LIB_DIRNAME = "local_model_library"
_MODELS_SUBDIR = "models"


def _normalize_key(item_name: str) -> str:
    """与 local_model_library._normalize_key 一致。"""
    return (item_name or "").strip().lower()


def _safe_dirname(key: str) -> str:
    """与 local_model_library._safe_dirname 一致。"""
    cleaned = re.sub(r"\s+", "_", (key or "").strip())
    cleaned = re.sub(r"[^0-9A-Za-z_\-一-鿿]", "_", cleaned)
    cleaned = cleaned.strip("_") or "model"
    h = hashlib.sha1((key or "").encode("utf-8")).hexdigest()[:8]
    return f"{cleaned[:48]}_{h}"


def _base_name(dirname: str) -> str:
    """去掉重复版本后缀 _N → 物体基名。如 '地毯_3' -> '地毯'，'坐垫' -> '坐垫'。"""
    return re.sub(r"_\d+$", "", dirname).strip()


def _version_num(dirname: str, base: str) -> int:
    """版本序号：无后缀(首次生成)=0，'base_N'=N。N 越大越新。"""
    m = re.match(re.escape(base) + r"_(\d+)$", dirname)
    return int(m.group(1)) if m else 0


def _first_model_file(d: str) -> str:
    """目录内第一个支持的模型文件（与 resolve_model_file 扫目录逻辑一致）。"""
    if not os.path.isdir(d):
        return ""
    for e in sorted(os.listdir(d)):
        if os.path.splitext(e)[1].lower() in _MODEL_EXTS:
            return os.path.join(d, e)
    return ""


def import_models(src: str, project: str, force: bool = False) -> None:
    if not os.path.isdir(src):
        raise SystemExit(f"源目录不存在: {src}")
    root = str(canonical_model_library_root(project))
    models_root = os.path.join(root, _MODELS_SUBDIR)
    os.makedirs(models_root, exist_ok=True)
    index_path = os.path.join(root, "index.json")

    index = {}
    if os.path.isfile(index_path):
        try:
            with open(index_path, "r", encoding="utf-8") as f:
                index = json.load(f) or {}
        except Exception:
            index = {}

    # 1. 收集源目录，按基名分组
    groups: dict[str, list[str]] = {}
    for name in sorted(os.listdir(src)):
        d = os.path.join(src, name)
        if not os.path.isdir(d):
            continue
        if not _first_model_file(d):
            print(f"  跳过(无模型文件): {name}")
            continue
        groups.setdefault(_base_name(name), []).append(name)

    # 2. 每个物体选最新版本（最大 _N；无后缀视为版本 0=最早）
    imported, skipped = 0, 0
    for base, members in sorted(groups.items()):
        rep = max(members, key=lambda d: _version_num(d, base))
        key = _normalize_key(base)
        if not key:
            continue
        if key in index and not force:
            existing = os.path.join(root, index[key].get("model_dir", ""))
            if _first_model_file(existing):
                print(f"  已存在,跳过: {base}  ({len(members)} 个版本)")
                skipped += 1
                continue

        safe = _safe_dirname(key)
        dst = os.path.join(models_root, safe)
        if os.path.isdir(dst):
            shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(os.path.join(src, rep), dst)
        index[key] = {
            "model_dir": os.path.join(_MODELS_SUBDIR, safe),
            "created_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        }
        print(f"  导入: {base}  <- {rep}(最新)  ({len(members)} 个版本)")
        imported += 1

    tmp = index_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=2)
    os.replace(tmp, index_path)

    print(f"\n完成: 导入 {imported}, 跳过 {skipped}, 库共 {len(index)} 个物体")
    print(f"库根: {root}")


def main() -> None:
    ap = argparse.ArgumentParser(description="批量导入模型到本地模型库")
    ap.add_argument("--src", required=True, help="源模型目录(其下每个子目录=一个物体)")
    ap.add_argument("--project", required=True, help="目标项目根路径(库写到 <project>/Resource/local_model_library/)")
    ap.add_argument("--force", action="store_true", help="覆盖已存在的同名条目")
    args = ap.parse_args()
    import_models(args.src, args.project, args.force)


if __name__ == "__main__":
    main()
