"""Read the active project from the current engine scene.

Session identity is pulled from C++ on every call: `archive_session` is minted by the
Python parser but owned by the scene afterwards, so a cached copy here would go stale the
moment the editor reopens the project.
"""

from __future__ import annotations

import logging
from pathlib import Path

logger = logging.getLogger(__name__)

_path_only_binding_warned = False


def _resolve_native_engine():
    try:
        import CoronaEngine
    except ImportError:
        return None
    return CoronaEngine


def read_project_session() -> dict:
    """Read the current session without creating a scene or hydrating launcher history."""
    global _path_only_binding_warned
    engine = _resolve_native_engine()
    if engine is None:
        return {}

    reader = getattr(engine, "active_project_session", None)
    if callable(reader):
        session = reader()
        return dict(session) if isinstance(session, dict) else {}

    # An older native module only exposes the path. Callers that need the session
    # identity (the archive/file guards) will refuse for safety; say so once so a
    # mismatched deployment is diagnosable instead of looking like a closed project.
    if not _path_only_binding_warned:
        _path_only_binding_warned = True
        logger.warning(
            "event=runtime.session.binding_missing binding=active_project_session; "
            "project requests cannot be validated against the open project")
    path = getattr(engine, "active_project_path", None)
    project_path = path() if callable(path) else ""
    return {"project_path": project_path} if project_path else {}


def get_active_project_path() -> str:
    return str(read_project_session().get("project_path") or "")


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
