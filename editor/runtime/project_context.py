"""Canonical runtime access to the active project context.

Project metadata and editor preferences remain owned by ``config.settings``.
This module only resolves the current project path for runtime consumers and
keeps the embedded-engine synchronization needed by legacy hosts.
"""

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


def get_active_project_path(native_engine=None) -> str:
    """Return the active project path without exposing a native engine object."""
    try:
        from config.project_state import settings_manager

        project_path = getattr(settings_manager, "active_project_path", None)
        if project_path:
            return str(project_path)
    except Exception:
        pass

    engine = _resolve_native_engine(native_engine)
    return str(getattr(engine, "active_project_path", "") or "")


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


def set_compat_active_project_path(project_path, native_engine=None):
    """Synchronize the path with an older embedded host, if available."""
    engine = _resolve_native_engine(native_engine)
    if engine is None:
        return False
    try:
        setattr(engine, "active_project_path", str(project_path or ""))
    except Exception:
        return False
    return True
