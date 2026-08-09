from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[2]


def test_script_runtime_uses_its_legacy_scene_adapter_boundary():
    engine_source = (
        EDITOR_ROOT / "script_runtime" / "engine" / "corona_engine.py"
    ).read_text(encoding="utf-8")
    blockly_source = (
        EDITOR_ROOT / "script_runtime" / "blockly" / "main.py"
    ).read_text(encoding="utf-8")
    adapter = EDITOR_ROOT / "runtime" / "legacy_script_scene_adapter.py"

    assert adapter.is_file()
    assert "runtime.legacy_scene_store" not in engine_source
    assert "script_runtime.compat.legacy_scene_adapter" in engine_source
    assert "runtime.legacy_scene_store" not in blockly_source
    assert "script_runtime.compat.legacy_scene_adapter" in blockly_source


def test_script_runtime_owns_the_legacy_scene_adapter_implementation():
    engine_source = (
        EDITOR_ROOT / "script_runtime" / "engine" / "corona_engine.py"
    ).read_text(encoding="utf-8")
    blockly_source = (
        EDITOR_ROOT / "script_runtime" / "blockly" / "main.py"
    ).read_text(encoding="utf-8")
    canonical = (
        EDITOR_ROOT / "script_runtime" / "compat" / "legacy_scene_adapter.py"
    )
    shim_source = (
        EDITOR_ROOT / "runtime" / "legacy_script_scene_adapter.py"
    ).read_text(encoding="utf-8")

    assert canonical.is_file()
    assert "script_runtime.compat.legacy_scene_adapter" in engine_source
    assert "script_runtime.compat.legacy_scene_adapter" in blockly_source
    assert "from script_runtime.compat.legacy_scene_adapter import *" in shim_source


def test_legacy_script_scene_adapter_delegates_store_operations(monkeypatch):
    import script_runtime.compat.legacy_scene_adapter as adapter

    class FakeStore:
        def get_all(self):
            return {"main": "scene"}

        def get(self, name):
            return f"get:{name}"

        def list_all(self):
            return ["main"]

        def get_or_create(self, route):
            return f"create:{route}"

        def find_actor(self, target):
            return f"actor:{target}"

    monkeypatch.setattr(adapter, "_scene_store", FakeStore())

    assert adapter.get_all_scenes() == {"main": "scene"}
    assert adapter.get_scene("main") == "get:main"
    assert adapter.list_scene_routes() == ["main"]
    assert adapter.get_or_create_scene("main") == "create:main"
    assert adapter.find_actor("hero") == "actor:hero"


def test_historical_runtime_import_remains_a_forwarding_alias(monkeypatch):
    import runtime.legacy_script_scene_adapter as legacy_alias
    import script_runtime.compat.legacy_scene_adapter as canonical

    class FakeStore:
        def get(self, name):
            return f"get:{name}"

    monkeypatch.setattr(canonical, "_scene_store", FakeStore())

    assert legacy_alias.get_scene("main") == "get:main"
