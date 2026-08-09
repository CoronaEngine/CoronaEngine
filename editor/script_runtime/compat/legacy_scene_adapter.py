"""Script Runtime compatibility boundary for the legacy Python scene store.

This module owns the compatibility contract used by generated scripts and
Blockly. It delegates storage to the legacy runtime store and does not define
new scene semantics.
"""

from runtime.legacy_scene_store import legacy_scene_store as _scene_store


def get_all_scenes():
    return _scene_store.get_all()


def get_scene(name):
    return _scene_store.get(name)


def list_scene_routes():
    return _scene_store.list_all()


def get_or_create_scene(route):
    return _scene_store.get_or_create(route)


def find_actor(target):
    return _scene_store.find_actor(target)


__all__ = [
    "find_actor",
    "get_all_scenes",
    "get_or_create_scene",
    "get_scene",
    "list_scene_routes",
]
