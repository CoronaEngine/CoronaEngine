"""AITool compatibility boundary for legacy Python scenes.

AITool should prefer native scene snapshots and value objects.  These helpers
remain only for old-host fallbacks and deliberately keep the legacy store out
of native scene parsing and view-model code.
"""

from typing import Any

def _store(manager: Any = None) -> Any:
    if manager is not None:
        return manager
    from runtime.legacy_scene_store import legacy_scene_store

    return legacy_scene_store


def get_legacy_scene(scene_name: str = "", *, manager: Any = None) -> Any:
    scene_store = _store(manager)
    if scene_name:
        scene = scene_store.get(scene_name)
        if scene is not None:
            return scene
        for route in scene_store.list_all():
            candidate = scene_store.get(route)
            if candidate is not None and getattr(candidate, "name", None) == scene_name:
                return candidate
    routes = scene_store.list_all()
    return scene_store.get(routes[0]) if routes else None


def list_legacy_scene_routes(*, manager: Any = None) -> list[str]:
    return list(_store(manager).list_all())


__all__ = ["get_legacy_scene", "list_legacy_scene_routes"]
