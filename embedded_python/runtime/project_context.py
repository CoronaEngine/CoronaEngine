"""Read the active project from the current engine scene."""

from __future__ import annotations

from pathlib import Path


def _resolve_native_engine():
    try:
        import CoronaEngine
    except ImportError:
        return None
    return CoronaEngine


def read_project_session() -> dict:
    """Read the current session without creating a scene or hydrating launcher history."""
    engine = _resolve_native_engine()
    if engine is None:
        return {}
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
