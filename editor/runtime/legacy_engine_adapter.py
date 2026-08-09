"""Narrow bridges from legacy Python adapters to the native runtime.

This module is reserved for legacy entities/components and explicitly
registered compatibility paths. Editor business code must use the
manifest-backed adapters in ``api.editor_api`` instead.
"""

from runtime.editor_host import CoronaEditor


CoronaEngine = CoronaEditor.CoronaEngine


class EditorEngineAdapter:
    """Internal native operations used by aggregate handlers."""

    def __init__(self, native_engine=None):
        self._engine = native_engine if native_engine is not None else CoronaEngine

    def _method(self, name):
        method = getattr(self._engine, name, None)
        if not callable(method):
            raise RuntimeError(f"native engine operation '{name}' is unavailable")
        return method

    @property
    def active_project_path(self):
        return getattr(self._engine, "active_project_path", None)

    def is_vision_available(self):
        return bool(self._method("is_vision_available")())

    def set_render_backend(self, mode):
        return self._method("set_render_backend")(mode)

    def get_render_backend(self):
        return self._method("get_render_backend")()

    def load_vision_scene(self, path):
        return self._method("load_vision_scene")(path)

    def create_editor_actor(self, scene_name, source_path, actor_type, actor_data):
        return self._method("create_editor_actor")(
            scene_name, source_path, actor_type, actor_data
        )

    @staticmethod
    def _camera_method(camera, name):
        method = getattr(camera, name, None)
        if callable(method):
            return method
        return getattr(getattr(camera, "engine_obj", None), name, None)

    def set_camera_shadow_cascade_debug(self, camera, enabled):
        setter = self._camera_method(camera, "set_shadow_cascade_debug")
        if callable(setter):
            setter(bool(enabled))

    def get_camera_shadow_cascade_debug(self, camera):
        getter = self._camera_method(camera, "get_shadow_cascade_debug")
        return bool(getter()) if callable(getter) else False

    def set_camera_ssao_enabled(self, camera, enabled):
        setter = self._camera_method(camera, "set_ssao_enabled")
        if callable(setter):
            setter(bool(enabled))

    def get_camera_ssao_enabled(self, camera):
        getter = self._camera_method(camera, "get_ssao_enabled")
        return bool(getter()) if callable(getter) else True


def get_editor_engine_adapter(native_engine=None):
    """Return the internal adapter for a handler or legacy host."""

    return EditorEngineAdapter(native_engine)
