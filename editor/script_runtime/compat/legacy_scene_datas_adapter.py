"""Legacy SceneDatas adapter restricted to Script Runtime callers.

This is a compatibility surface, not a second scene contract. The manifest
method names and caller permissions remain owned by the C++ editor manifest.
"""


class LegacySceneDatasApi:
    """Expose the legacy SceneDatas surface through an injected invoker."""

    def __init__(self, invoke_manifest):
        self._invoke_manifest = invoke_manifest

    def get_scene(self, scene_name=""):
        return self._invoke_manifest("scene_datas.get_scene", [scene_name])

    def get_actor(self, scene_name, actor_name):
        return self._invoke_manifest("scene_datas.get_actor", [scene_name, actor_name])

    def actor_operation(self, scene_name, actor_name, operation, vector=None):
        return self._invoke_manifest(
            "scene_datas.actor_operation",
            [scene_name, actor_name, operation, vector],
        )

    def save_actor(self, scene_name, actor_name):
        return self._invoke_manifest("scene_datas.save_actor", [scene_name, actor_name])

    def select_model_file(self, scene_name, actor_name, file_type=""):
        return self._invoke_manifest(
            "scene_datas.select_model_file",
            [scene_name, actor_name, file_type],
        )


__all__ = ["LegacySceneDatasApi"]
