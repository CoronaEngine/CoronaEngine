from pathlib import Path
import sys


CORE_ROOT = Path(__file__).resolve().parents[1].parent / "CoronaCore" / "core"
REPO_ROOT = CORE_ROOT.parents[2]
sys.path.insert(0, str(REPO_ROOT / "editor"))

from CoronaCore.core.legacy_scene_store import LegacySceneStore
from runtime import legacy_scene_store as runtime_scene_store_module


def test_scene_host_plugins_do_not_import_scene_manager_directly():
    for relative_path in (
        "editor/plugins/MainView/main.py",
        "editor/backend/file_system/main.py",
        "editor/script_runtime/blockly/main.py",
        "editor/script_runtime/engine/corona_engine.py",
        "editor/plugins/SceneTools/main.py",
        "editor/CoronaCore/core/corona_editor.py",
    ):
        source = (REPO_ROOT / relative_path).read_text(encoding="utf-8")
        assert "from CoronaCore.core.legacy.managers import scene_manager" not in source


def test_file_system_uses_native_files_handler_for_file_operations():
    source = (REPO_ROOT / "editor/plugins/FileManager/main.py").read_text(encoding="utf-8")
    adapter = (REPO_ROOT / "editor/plugins/FileManager/compat/legacy_file_scene_adapter.py").read_text(
        encoding="utf-8"
    )

    assert "from CoronaCore.core.legacy.entities import Actor" not in source
    assert "from api.editor_api import CoronaEditorApi" in source
    assert "CoronaEditorApi.files" in source
    assert "open_file_binding" in adapter


def test_legacy_scene_store_creates_compatibility_actor_lazily(monkeypatch):
    import types

    created = []

    class FakeActor:
        def __init__(self, **kwargs):
            created.append(kwargs)
            self.route = kwargs["route"]

    fake_entities = types.ModuleType("runtime.legacy.entities")
    fake_entities.Actor = FakeActor
    monkeypatch.setitem(sys.modules, "runtime.legacy.entities", fake_entities)

    actor = LegacySceneStore().create_actor("scene/actor.actor", "actor")

    assert actor.route == "scene/actor.actor"
    assert created == [{"route": "scene/actor.actor", "actor_type": "actor"}]


def test_legacy_scene_store_is_the_explicit_scene_manager_boundary():
    source = (REPO_ROOT / "editor" / "runtime" / "legacy_scene_store.py").read_text(
        encoding="utf-8"
    )
    assert "from runtime.legacy.managers import scene_manager" in source
    assert "class LegacySceneStore" in source


def test_runtime_owns_legacy_scene_store_and_core_path_is_compatibility_alias():
    runtime_source = (REPO_ROOT / "editor" / "runtime" / "legacy_scene_store.py").read_text(
        encoding="utf-8"
    )
    compatibility_source = (CORE_ROOT / "legacy_scene_store.py").read_text(encoding="utf-8")

    assert "class LegacySceneStore" in runtime_source
    assert "Compatibility" in compatibility_source
    assert "from runtime.legacy_scene_store import" in compatibility_source
    assert runtime_scene_store_module.LegacySceneStore is LegacySceneStore


def test_scene_host_production_code_uses_runtime_scene_store_path():
    for relative_path in (
        "editor/runtime/editor_host.py",
        "editor/plugins/MainView/main.py",
        "editor/plugins/FileManager/main.py",
        "editor/plugins/SceneTools/main.py",
        "editor/plugins/AITool/cai_extensions/mcp/tools/native_scene_state.py",
        "editor/script_runtime/blockly/main.py",
        "editor/script_runtime/engine/corona_engine.py",
    ):
        source = (REPO_ROOT / relative_path).read_text(encoding="utf-8")
        assert "CoronaCore.core.legacy_scene_store" not in source


def test_script_runtime_host_uses_active_owner_and_scene_adapter():
    source = (
        REPO_ROOT / "editor" / "script_runtime" / "engine" / "host.py"
    ).read_text(encoding="utf-8")
    assert "from runtime.legacy_scene_store import" not in source
    assert "from script_runtime.compat.legacy_scene_adapter import" in source
    assert not (
        REPO_ROOT / "editor" / "script_runtime" / "compat" / "legacy_script_runtime_adapter.py"
    ).exists()


def test_camera_follow_uses_explicit_legacy_scene_store_name():
    source = (REPO_ROOT / "editor" / "runtime" / "legacy_camera_follow.py").read_text(
        encoding="utf-8"
    )

    assert "legacy_scene_store as scene_manager" not in source
    assert "from runtime.legacy_scene_store import legacy_scene_store" in source


def test_legacy_scene_store_forwards_only_compatibility_operations(monkeypatch):
    calls = []

    class FakeSceneManager:
        def get(self, value):
            calls.append(("get", value))
            return "scene"

        def get_or_create(self, value):
            calls.append(("get_or_create", value))
            return "created"

        def remove(self, value):
            calls.append(("remove", value))
            return True

        def register(self, value, scene):
            calls.append(("register", value, scene))

        def find_actor_by_route(self, value):
            calls.append(("find_actor_by_route", value))
            return "actor"

        def list_all(self):
            calls.append(("list_all",))
            return ["a"]

        def get_all(self):
            calls.append(("get_all",))
            return {"a": "scene"}

    monkeypatch.setattr(runtime_scene_store_module, "_scene_manager", FakeSceneManager())
    store = LegacySceneStore()

    assert store.get("a") == "scene"
    assert store.get_or_create("b") == "created"
    assert store.remove("b") is True
    store.register("c", "scene")
    assert store.find_actor_by_route("d") == "actor"
    assert store.list_all() == ["a"]
    assert store.get_all() == {"a": "scene"}
    assert calls == [
        ("get", "a"),
        ("get_or_create", "b"),
        ("remove", "b"),
        ("register", "c", "scene"),
        ("find_actor_by_route", "d"),
        ("list_all",),
        ("get_all",),
    ]


def test_engine_entities_use_one_runtime_adapter_for_legacy_host_access():
    engine_files = [
        "editor/runtime/legacy/entities/actor.py",
        "editor/runtime/legacy/entities/camera.py",
        "editor/runtime/legacy/entities/environment.py",
        "editor/runtime/legacy/components/acoustics.py",
        "editor/runtime/legacy/components/geometry.py",
        "editor/runtime/legacy/components/kinematics.py",
        "editor/runtime/legacy/components/mechanics.py",
        "editor/runtime/legacy/components/optics.py",
    ]
    for relative_path in engine_files:
        source = (REPO_ROOT / relative_path).read_text(encoding="utf-8")
        assert "from ..corona_editor import CoronaEditor" not in source
        assert "runtime.legacy_engine_adapter import CoronaEngine" in source

    scene_source = (REPO_ROOT / "editor/runtime/legacy/entities/scene.py").read_text(
        encoding="utf-8"
    )
    assert "from ..engine_runtime import CoronaEngine" not in scene_source
    assert "get_scene_adapter" in scene_source
    assert "get_scene_tools_adapter" in scene_source
    assert "get_viewport_adapter" in scene_source

    for relative_path in (
        "editor/runtime/legacy/entities/actor.py",
        "editor/runtime/legacy/entities/scene.py",
    ):
        source = (REPO_ROOT / relative_path).read_text(encoding="utf-8")
        assert "CoronaEditor.emit_editor_event" not in source
        assert "emit_compat_editor_event" in source

    runtime_source = (REPO_ROOT / "editor/runtime/legacy_engine_adapter.py").read_text(
        encoding="utf-8"
    )
    assert "from runtime.editor_host import CoronaEditor" in runtime_source
