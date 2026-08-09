from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
MAIN_SOURCE = PLUGIN_ROOT / "main.py"
VISION_ADAPTER = PLUGIN_ROOT / "compat" / "legacy_vision_scene_adapter.py"
VISION_IMPORT_ADAPTER = PLUGIN_ROOT / "vision_import.py"
COMPAT_IMPORT_ADAPTER = PLUGIN_ROOT / "compat" / "legacy_vision_import_adapter.py"


def test_scene_tools_has_a_local_boundary_document():
    boundary = PLUGIN_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    assert "scene.*" in source
    assert "sceneTools.*" in source
    assert "native scene snapshot" in source
    assert "已删除 `legacy_vision_import_helper.py`" in source


def test_scene_tools_historical_facade_is_removed_after_native_migration():
    registry = (PLUGIN_ROOT.parents[1] / "runtime" / "registry.py").read_text(
        encoding="utf-8"
    )

    assert not (PLUGIN_ROOT / "compat" / "legacy_scene_tools.py").exists()
    assert "class SceneTools" in MAIN_SOURCE.read_text(encoding="utf-8")
    assert '"plugins.SceneTools.main", "SceneTools"' in registry


def test_scene_tools_is_the_aggregate_owner_and_isolates_vision_legacy_access():
    main_source = MAIN_SOURCE.read_text(encoding="utf-8")
    import_adapter_source = VISION_IMPORT_ADAPTER.read_text(encoding="utf-8")

    assert "from api.editor_api import CoronaEditorApi" in main_source
    assert "@PluginBase.register_web(\"SceneTools\")" in main_source
    assert "from plugins.SceneTools.vision_import import" in main_source
    assert "get_legacy_vision_scene" not in main_source
    assert not VISION_ADAPTER.exists()
    assert "get_legacy_vision_scene" not in import_adapter_source
    assert "plugins.SceneTools.vision_binding_sync" in import_adapter_source
    assert not COMPAT_IMPORT_ADAPTER.exists()
    assert "CoronaEditorApi.scene.get_snapshot" in import_adapter_source
    assert "CoronaEditorApi.main.scene_save" in import_adapter_source
    assert "CoronaEditorApi.scene_tools.remove_actor" in import_adapter_source
    assert "scene_manager" not in main_source
    assert "CoronaCore" not in main_source


def test_runtime_vision_paths_remain_forwarding_aliases():
    assert not (PLUGIN_ROOT.parents[1] / "runtime" / "legacy_vision_import_adapter.py").exists()
    assert not (PLUGIN_ROOT.parents[1] / "runtime" / "legacy_vision_scene_adapter.py").exists()


def test_unreferenced_vision_helper_has_been_removed():
    assert not (PLUGIN_ROOT / "compat" / "legacy_vision_import_helper.py").exists()
    assert VISION_IMPORT_ADAPTER.is_file()
