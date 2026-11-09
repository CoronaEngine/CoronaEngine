"""Single-scene archive loading facade.

Archive format parsing belongs to :mod:`runtime.archive`; this plugin owns
only the request facade and its load-policy/diagnostic mapping.
"""

from __future__ import annotations

import configparser
from pathlib import Path
from uuid import uuid4

from runtime.archive.errors import ArchiveParseError
from runtime.archive.parser import parse_archive
from runtime.archive.portable import recover_scene, validate_assets
from runtime.archive.writer import save_scene
from runtime.archive.project import create_project, project_info, update_metadata, validate_project
from runtime.archive.assets import asset_operation
from runtime.archive.documents import read_document, commit_document
from runtime.archive.editor_settings import editor_settings
from runtime.archive.project_files import file_manager
from runtime.plugin_base import PluginBase


@PluginBase.register_web("ProjectArchive")
class ProjectArchive(PluginBase):
    """Internal Script Service facade for the pure archive parser."""

    asset_operation = staticmethod(asset_operation)
    read_document = staticmethod(read_document)
    commit_document = staticmethod(commit_document)
    editor_settings = staticmethod(editor_settings)
    file_manager = staticmethod(file_manager)
    create_project = staticmethod(create_project)
    project_info = staticmethod(project_info)
    validate_project = staticmethod(validate_project)

    @staticmethod
    def update_metadata(request):
        from script_runtime.file_runner import FileScripts
        if FileScripts.has_restore_snapshot():
            raise ValueError("请先停止脚本并恢复场景，再保存项目设置")
        return update_metadata(request)

    @staticmethod
    def parse(payload: dict | str) -> dict:
        request = payload if isinstance(payload, dict) else {"path": str(payload)}
        path = str(request.get("path") or "").strip()
        load_policy = str(request.get("load_policy") or "prompt").strip().lower()
        if load_policy not in {"prompt", "degraded"}:
            return {
                "ok": False,
                "status": "invalid_archive",
                "diagnostics": [
                    {
                        "severity": "error",
                        "recoverable": False,
                        "stage": "archive_parse",
                        "code": "INVALID_LOAD_POLICY",
                        "message": f"Unsupported load policy: {load_policy}",
                        "path": path,
                    }
                ],
            }
        try:
            source = Path(path)
            if source.is_dir() or source.name == "scene.ini":
                recover_scene((source if source.is_dir() else source.parent).resolve())
            snapshot = parse_archive(path)
            snapshot["archive_session"] = uuid4().hex
            if snapshot.get("archive_type") == "portable_scene":
                missing = validate_assets(Path(snapshot["project_root"]), snapshot["extensions"]["ini_sections"],
                                          verify_hashes=True)
                snapshot["diagnostics"].extend(missing)
        except ArchiveParseError as exc:
            return {
                "ok": False,
                "status": "invalid_archive",
                "diagnostics": [exc.to_diagnostic()],
            }

        diagnostics = list(snapshot.get("diagnostics") or [])
        requires_confirmation = any(
            item.get("severity", "error") == "error" or item.get("requires_confirmation", True)
            for item in diagnostics
        )
        if requires_confirmation and load_policy == "prompt":
            status = "decision_required"
        elif requires_confirmation:
            status = "ready_degraded"
        else:
            status = "ready"
        return {
            "ok": True,
            "status": status,
            "snapshot": snapshot,
            "diagnostics": diagnostics,
        }

    @staticmethod
    def save(request: dict) -> dict:
        from script_runtime.file_runner import FileScripts

        if FileScripts.has_restore_snapshot():
            return {"ok": False, "status": "run_active", "message": "请先停止脚本并恢复场景，再保存存档"}
        try:
            return save_scene(request)
        except ArchiveParseError as exc:
            return {"ok": False, "status": "error", "message": str(exc), "diagnostics": [exc.to_diagnostic()]}
        except (KeyError, TypeError, ValueError, OSError, configparser.Error) as exc:
            return {"ok": False, "status": "error", "message": str(exc)}


__all__ = ["ProjectArchive"]
