"""Read the active project from the current engine scene."""

from __future__ import annotations

import os
from pathlib import Path


def _resolve_native_engine(native_engine=None):
    if native_engine is not None:
        return native_engine
    try:
        import CoronaEngine
    except ImportError:
        return None
    return CoronaEngine


def read_project_session() -> dict:
    """Read the current session without creating a scene or hydrating launcher history."""
    if _resolve_native_engine() is None:
        return {}
    from api.editor_api import get_script_runtime_editor_api

    return get_script_runtime_editor_api().scene.read_active()


def get_active_project_path() -> str:
    return str(read_project_session().get("project_path") or "")


def is_native_engine_available(native_engine=None) -> bool:
    """Report host availability without exposing the native engine object."""
    return _resolve_native_engine(native_engine) is not None


def get_project_root(*, fallback_to_environment=True) -> Path:
    """Resolve a filesystem root for runtime tools that need a safe fallback."""
    project_path = get_active_project_path()
    if project_path:
        return Path(project_path).expanduser()

    if fallback_to_environment:
        configured = (os.environ.get("CORONAENGINE_PROJECT") or "").strip()
        if configured:
            return Path(configured).expanduser()
    return Path.cwd()


def require_project_session(data: dict) -> tuple[Path | None, dict | None]:
    state = read_project_session()
    if not state.get("archive_session") or not state.get("project_path"):
        return None, {"status": "error", "code": "NO_ACTIVE_PROJECT", "message": "No project is open"}
    root = Path(state["project_path"]).resolve()
    if (data.get("archive_session") != state["archive_session"]
            or not data.get("project_path") or Path(data["project_path"]).resolve() != root):
        return None, {"status": "error", "code": "PROJECT_CONTEXT_CHANGED",
                      "message": "Project session changed; request was ignored"}
    if state.get("read_only"):
        return None, {"status": "error", "code": "READ_ONLY_PROJECT", "message": "Project is read-only"}
    return root, None
