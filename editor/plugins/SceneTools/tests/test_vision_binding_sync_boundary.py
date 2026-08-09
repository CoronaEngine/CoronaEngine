import importlib
from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
MAIN_SOURCE = (PLUGIN_ROOT / "compat" / "legacy_scene_tools.py").read_text(encoding="utf-8")
CANONICAL_PATH = PLUGIN_ROOT / "compat" / "legacy_vision_binding_sync.py"
LEGACY_PATH = PLUGIN_ROOT / "vision_binding_sync.py"


def test_vision_binding_sync_has_a_dedicated_owner():
    source_path = CANONICAL_PATH
    assert source_path.is_file()
    source = source_path.read_text(encoding="utf-8")
    shim = LEGACY_PATH.read_text(encoding="utf-8")
    for symbol in (
        "find_actor_by_guid",
        "sync_external_live_binding_source_path",
        "remove_stale_vision_proxy_actors",
    ):
        assert f"def {symbol}" in source
        assert f"def _{symbol}" not in MAIN_SOURCE
    assert "from plugins.SceneTools.compat.legacy_vision_binding_sync import" in shim


def test_vision_binding_sync_updates_and_removes_legacy_actors():
    sync = importlib.import_module("plugins.SceneTools.vision_binding_sync")
    canonical = importlib.import_module(
        "plugins.SceneTools.compat.legacy_vision_binding_sync"
    )
    assert sync.find_actor_by_guid is canonical.find_actor_by_guid

    class Actor:
        def __init__(self, guid):
            self.actor_guid = guid
            self.name = guid
            self.binding = None
            self.cleared = False

        def set_external_vision_binding(self, binding):
            self.binding = binding

        def clear_external_vision_binding(self):
            self.cleared = True

    class Scene:
        def __init__(self):
            self.actors = [Actor("active"), Actor("stale")]

        def get_actors(self):
            return self.actors

        def remove_actor(self, actor):
            self.actors.remove(actor)

    scene = Scene()
    stale_actor = scene.actors[1]
    sync.sync_external_live_binding_source_path(
        scene, "/tmp/vision.json", [{"actor_guid": "active", "json_path": "/scene/shapes/0"}]
    )
    assert scene.actors[0].binding["source_path"] == "/tmp/vision.json"
    assert sync.remove_stale_vision_proxy_actors(
        scene, [{"actor_guid": "stale"}], {"active"}
    ) == 1
    assert stale_actor.cleared is True
    assert [actor.actor_guid for actor in scene.actors] == ["active"]
