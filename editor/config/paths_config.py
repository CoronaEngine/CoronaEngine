"""
路径配置
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
    generated_script_dir: Path
    autosave_dir: Path
    config_dir: Path
    assets_model_dir: Path
    object_recognition_db: Path
    screenshots_dir: Optional[Path] = None
    media_local_storage: Optional[Path] = None

def get_default_paths() -> PathsConfig:
    """获取默认路径配置"""
    # 从当前文件位置计算项目根目录
    repo_root = Path(__file__).resolve().parents[1]
    config_dir = repo_root / "config"
    autosave_dir = get_project_media_dir()
    assets_model_dir = get_project_models_dir()
    object_recognition_db = get_project_recognition_db()

    return PathsConfig(
        repo_root=repo_root,
        frontend_dist=repo_root / "Frontend" / "dist" / "index.html",
        generated_script_dir=repo_root / "runtime" / "generated",
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
    """获取当前项目的媒体存储目录: <project_path>/media/"""
    d = _get_active_project_path() / "media"
    d.mkdir(parents=True, exist_ok=True)
    return d


def get_project_models_dir() -> Path:
    """获取当前项目的模型目录: <project_path>/models/"""
    d = _get_active_project_path() / "models"
    d.mkdir(parents=True, exist_ok=True)
    return d


def get_project_screenshots_dir() -> Path:
    """获取当前项目的截图目录: <project_path>/screenshots/"""
    d = _get_active_project_path() / "screenshots"
    d.mkdir(parents=True, exist_ok=True)
    return d


def get_project_recognition_db() -> Path:
    """获取当前项目的物体识别数据库路径: <project_path>/models/database.db"""
    models_dir = get_project_models_dir()
    return models_dir / "database.db"


def get_repository_assets_dir() -> Path:
    """Return the repository-bundled asset root (read-only runtime fallback)."""
    return get_default_paths().repo_root.parent / "assets"
