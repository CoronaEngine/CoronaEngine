"""Restricted native capabilities owned by the Script Runtime boundary.

These operations are Engine Runtime primitives for role scripts and
Blockly/Scratch. They are intentionally separate from the editor manifest
adapter and must not be exposed to Vue or ordinary editor Python services.
"""


def _resolve_native_engine(native_engine=None):
    if native_engine is not None:
        return native_engine
    try:
        import CoronaEngine
    except ImportError:
        return None
    return CoronaEngine


class ScriptRuntimeNativeEngineAdapter:
    """Expose only the native operations allowed to Script Runtime code."""

    def __init__(self, native_engine):
        self._native_engine = native_engine

    def set_mouse_locked(self, enabled):
        method = getattr(self._native_engine, "set_mouse_locked", None)
        return method(bool(enabled)) if callable(method) else None

    def get_mouse_delta(self):
        method = getattr(self._native_engine, "get_mouse_delta", None)
        return method() if callable(method) else None

    def ray_cast(self, origin, direction, max_dist):
        method = getattr(self._native_engine, "ray_cast", None)
        return method(origin, direction, float(max_dist)) if callable(method) else None

    def import_media(self, path):
        method = getattr(self._native_engine, "import_media", None)
        return method(path) if callable(method) else None

    def play_audio(self, resource_id, *, loop=False):
        method = getattr(self._native_engine, "play_audio", None)
        return method(resource_id, loop=bool(loop)) if callable(method) else None

    def stop_audio(self, resource_id):
        method = getattr(self._native_engine, "stop_audio", None)
        return method(resource_id) if callable(method) else None


def get_script_runtime_adapter(native_engine=None, *, resolver=None):
    """Return the restricted Script Runtime native adapter when available."""
    resolve = resolver or _resolve_native_engine
    native_engine = resolve(native_engine)
    if native_engine is None:
        return None
    if any(
        callable(getattr(native_engine, name, None))
        for name in (
            "set_mouse_locked",
            "get_mouse_delta",
            "ray_cast",
            "import_media",
            "play_audio",
            "stop_audio",
        )
    ):
        return ScriptRuntimeNativeEngineAdapter(native_engine)
    return None
