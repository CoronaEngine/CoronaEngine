"""Legacy Scene lookup used by the Vision import compatibility path."""

from runtime.legacy_scene_store import legacy_scene_store


def get_legacy_vision_scene(scene_name: str):
    """Resolve a legacy Python Scene for Vision import fallback operations."""
    return legacy_scene_store.get(scene_name)
