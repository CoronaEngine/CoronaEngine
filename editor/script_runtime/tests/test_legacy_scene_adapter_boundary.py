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

    assert not adapter.is_file()
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
    assert canonical.is_file()
    assert "script_runtime.compat.legacy_scene_adapter" in engine_source
    assert "script_runtime.compat.legacy_scene_adapter" in blockly_source


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


def test_historical_runtime_scene_shim_is_removed():
    assert not (EDITOR_ROOT / "runtime" / "legacy_script_scene_adapter.py").exists()
