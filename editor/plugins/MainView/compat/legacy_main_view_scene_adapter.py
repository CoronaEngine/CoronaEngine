"""Compatibility boundary for MainView's legacy Python scene lifecycle."""

import logging

from runtime.legacy_scene_store import legacy_scene_store


logger = logging.getLogger(__name__)


def get_or_create_scene(route):
    return legacy_scene_store.get_or_create(route)


def get_scene(route):
    return legacy_scene_store.get(route)


def remove_scene(route):
    scene = legacy_scene_store.get(route)
    if scene is None:
        return
    scene.set_enabled(False)
    legacy_scene_store.remove(route)


def discard_scene(route):
    scene = get_scene(route)
    if scene is not None:
        try:
            scene.set_enabled(False)
        except Exception:
            logger.debug("Failed to disable Python runtime scene %s", route, exc_info=True)
    try:
        legacy_scene_store.remove(route)
    except Exception:
        logger.debug("Failed to discard Python runtime scene %s", route, exc_info=True)
