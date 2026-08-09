"""Compatibility synchronization for Vision-backed legacy actors.

This module owns the small amount of Scene/Actor interaction needed to keep
legacy Vision proxy actors aligned with the manifest binding data.  It is not
the owner of the binding schema or the authoritative scene state.
"""

import logging


logger = logging.getLogger(__name__)


def find_actor_by_guid(scene, actor_guid: str):
    """Find a legacy actor by its stable GUID."""
    if scene is None or not actor_guid:
        return None
    try:
        for actor in scene.get_actors():
            if getattr(actor, "actor_guid", "") == actor_guid:
                return actor
    except Exception:
        pass
    try:
        return scene.find_actor(actor_guid)
    except Exception:
        return None


def sync_external_live_binding_source_path(scene, runtime_path: str, bindings=None) -> None:
    """Publish the derived runtime path to each compatible legacy actor."""
    if not runtime_path:
        return
    if bindings is None:
        bindings = getattr(scene, "vision_bindings", [])
    for binding in list(bindings or []):
        actor = find_actor_by_guid(scene, binding.get("actor_guid", ""))
        if actor is None or not hasattr(actor, "set_external_vision_binding"):
            continue
        runtime_binding = dict(binding)
        runtime_binding["source_path"] = runtime_path
        actor.set_external_vision_binding(runtime_binding)


def remove_stale_vision_proxy_actors(scene, previous_bindings, active_actor_guids) -> int:
    """Remove legacy proxy actors no longer represented by active bindings."""
    removed = 0
    active_actor_guids = set(active_actor_guids or ())
    for binding in previous_bindings or []:
        actor_guid = binding.get("actor_guid", "")
        if not actor_guid or actor_guid in active_actor_guids:
            continue
        actor = find_actor_by_guid(scene, actor_guid)
        if actor is None:
            continue
        try:
            clear_binding = getattr(actor, "clear_external_vision_binding", None)
            if callable(clear_binding):
                clear_binding()
            remove_actor = getattr(scene, "remove_actor", None)
            if callable(remove_actor):
                remove_actor(actor)
                removed += 1
        except Exception as exc:
            logger.warning(
                "Failed to remove stale Vision proxy actor %s: %s",
                getattr(actor, "name", actor_guid),
                exc,
            )
    return removed
