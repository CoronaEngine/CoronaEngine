"""Compatibility adapters for legacy editor host context and engine access.

The public contract remains owned by ``api.editor_api``. These helpers only
resolve or translate old embedded-host state and must not define manifest
methods, schemas, or authoritative editor state.
"""

from script_runtime.native_engine_adapter import (
    get_script_runtime_adapter as _get_script_runtime_adapter,
)
from runtime.project_context import (
    get_active_project_path,
    set_compat_active_project_path,
)


def _resolve_native_engine(native_engine=None):
    """Resolve the embedded host only inside the adapter boundary."""
    if native_engine is not None:
        return native_engine
    try:
        import CoronaEngine
    except ImportError:
        return None
    return CoronaEngine


def is_native_engine_available(native_engine=None):
    """Report host availability without exposing the native engine object."""
    return _resolve_native_engine(native_engine) is not None


def get_script_runtime_adapter(native_engine=None):
    """Compatibility entry point for the canonical Script Runtime adapter."""
    return _get_script_runtime_adapter(native_engine, resolver=_resolve_native_engine)


def emit_compat_editor_event(event_name, args=None):
    """Forward a legacy script event through the centralized compatibility bridge."""
    from runtime.editor_host import CoronaEditor

    return CoronaEditor.emit_editor_event(event_name, args)


def set_compat_editor_camera_input_enabled(enabled, *, reason=""):
    """Use the legacy reason-aware camera input gate behind the adapter boundary."""
    from runtime.editor_host import CoronaEditor

    setter = getattr(CoronaEditor, "set_editor_camera_input_enabled", None)
    if not callable(setter):
        raise RuntimeError("editor camera input compatibility gate is unavailable")
    return setter(bool(enabled), reason=reason)


def get_compat_editor_selection():
    """Read legacy editor selection state for Script Runtime restore notifications."""
    from runtime.editor_host import CoronaEditor

    return (
        getattr(CoronaEditor, "_selected_scene", None),
        getattr(CoronaEditor, "_selected_actor", None),
    )
