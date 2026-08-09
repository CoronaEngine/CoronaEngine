"""Compatibility boundary for the embedded Python scene host.

The Python scene manager is retained for the legacy scene-loading/runtime
path.  Editor plugins must access it through this small adapter so the
compatibility dependency stays in one place and can later be replaced by
manifest-backed scene lifecycle commands without changing every plugin.
"""

from runtime.legacy.managers import scene_manager as _scene_manager


class LegacySceneStore:
    """Explicit adapter for the pre-manifest Python scene store."""

    def get(self, scene_name):
        return _scene_manager.get(scene_name)

    def get_or_create(self, scene_name):
        return _scene_manager.get_or_create(scene_name)

    def remove(self, scene_name):
        return _scene_manager.remove(scene_name)

    def register(self, scene_name, scene):
        return _scene_manager.register(scene_name, scene)

    def find_actor_by_route(self, route):
        return _scene_manager.find_actor_by_route(route)

    def create_actor(self, route, actor_type="actor"):
        """Create an entity for an old host without exposing it to plugins."""
        from runtime.legacy.entities import Actor

        return Actor(route=route, actor_type=actor_type)

    def list_all(self):
        return _scene_manager.list_all()

    def get_all(self):
        return _scene_manager.get_all()


legacy_scene_store = LegacySceneStore()
