"""CAI 路径解析器的宿主实现。

转发到 runtime project context 和 ``editor/config/paths_config`` 中已有的实现，
不在此处重复编辑器/引擎逻辑。
"""

from __future__ import annotations

from pathlib import Path

from Quasar.ai_config.paths_config import PathsConfig


class CabbageEditorPathsResolver:
    """实现 CAI 的 ``PathsResolver`` 协议（duck typing）。"""

    def get_active_project_path(self) -> Path:
        from runtime.project_context import get_project_root

        return get_project_root()

    def get_project_media_dir(self) -> Path:
        from config.paths_config import get_project_media_dir
        return get_project_media_dir()

    def get_project_models_dir(self) -> Path:
        from config.paths_config import get_project_models_dir
        return get_project_models_dir()

    def get_project_screenshots_dir(self) -> Path:
        from config.paths_config import get_project_screenshots_dir
        return get_project_screenshots_dir()

    def get_project_recognition_db(self) -> Path:
        from config.paths_config import get_project_recognition_db
        return get_project_recognition_db()

    def get_default_paths(self) -> PathsConfig:
        from config.paths_config import get_default_paths
        editor_paths = get_default_paths()
        # 只注入 CAI 所需的通用项目路径，宿主内部路径不泄漏到 AITool 配置中。
        return PathsConfig(
            repo_root=editor_paths.repo_root,
            autosave_dir=editor_paths.autosave_dir,
            config_dir=editor_paths.config_dir,
            assets_model_dir=editor_paths.assets_model_dir,
            object_recognition_db=editor_paths.object_recognition_db,
            screenshots_dir=getattr(editor_paths, "screenshots_dir", None),
            media_local_storage=getattr(editor_paths, "media_local_storage", None),
        )
