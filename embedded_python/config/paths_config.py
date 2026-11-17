"""
路径配置

只解析路径,不接触文件系统:这里的函数都不创建目录。需要目录的写入点在自己的
落盘路径上 ``mkdir(parents=True, exist_ok=True)``。启动期 import 不写盘是硬约束
——程序装在只读目录里也必须能启动。
"""

from dataclasses import dataclass
import os
from pathlib import Path
from typing import Optional


@dataclass(frozen=True)
class PathsConfig:
    """路径配置"""

    repo_root: Path
    frontend_dist: Path
    autosave_dir: Path
    config_dir: Path
    assets_model_dir: Path
    object_recognition_db: Path
    screenshots_dir: Optional[Path] = None
    media_local_storage: Optional[Path] = None

def get_default_paths() -> PathsConfig:
    """获取默认路径配置(纯解析,不在磁盘上留下任何东西)"""
    # 从当前文件位置计算项目根目录
    repo_root = Path(__file__).resolve().parents[1]
    config_dir = repo_root / "config"
    autosave_dir = get_project_media_dir()
    assets_model_dir = get_project_models_dir()
    object_recognition_db = get_project_recognition_db()

    return PathsConfig(
        repo_root=repo_root,
        frontend_dist=repo_root / "Frontend" / "dist" / "index.html",
        autosave_dir=autosave_dir,
        config_dir=config_dir,
        assets_model_dir=assets_model_dir,
        object_recognition_db=object_recognition_db,
        media_local_storage=autosave_dir,
    )


# ---------------------------------------------------------------------------
# 基于活跃项目路径的动态目录解析
# ---------------------------------------------------------------------------

def _get_active_project_path() -> Path:
    """获取当前活跃项目路径，未打开项目时回退到 cwd。"""
    try:
        from runtime.project_context import get_active_project_path

        project_path = get_active_project_path()
        if project_path:
            return Path(project_path)
    except Exception:
        pass
    return Path(os.getcwd())


def get_project_media_dir() -> Path:
    """返回当前项目的媒体目录 <project_path>/media/;不创建它。"""
    return _get_active_project_path() / "media"


def get_project_models_dir() -> Path:
    """返回当前项目的模型目录 <project_path>/models/;不创建它。"""
    return _get_active_project_path() / "models"


def get_project_screenshots_dir() -> Path:
    """返回当前项目的截图目录 <project_path>/screenshots/;不创建它。"""
    return _get_active_project_path() / "screenshots"


def get_project_recognition_db() -> Path:
    """返回当前项目的物体识别数据库路径 <project_path>/models/database.db;不创建它。"""
    models_dir = get_project_models_dir()
    return models_dir / "database.db"


def get_repository_assets_dir() -> Path:
    """Return the repository-bundled asset root (read-only runtime fallback)."""
    return get_default_paths().repo_root.parent / "assets"
