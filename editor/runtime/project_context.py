"""Canonical runtime access to the active project context.

Project metadata and editor preferences remain owned by ``config.project_state``.
This module resolves the current project path and synchronizes the embedded
native host. The native host path is authoritative; persisted project state is
used when no native context is available.
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
    engine = _resolve_native_engine(native_engine)
    native_path = str(getattr(engine, "active_project_path", "") or "")
    if native_path:
        return native_path

    try:
        from config.project_state import settings_manager

        project_path = getattr(settings_manager, "active_project_path", None)
        if project_path:
            return str(project_path)
    except Exception:
        pass

    return ""


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


def set_active_project_path(project_path, native_engine=None):
    """Synchronize the active path with the embedded native host."""
    engine = _resolve_native_engine(native_engine)
    if engine is None:
        return False
    try:
        setattr(engine, "active_project_path", str(project_path or ""))
    except Exception:
        return False
    return True
