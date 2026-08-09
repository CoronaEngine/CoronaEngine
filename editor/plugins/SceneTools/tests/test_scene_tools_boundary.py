from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
MAIN_SOURCE = PLUGIN_ROOT / "main.py"
VISION_ADAPTER = PLUGIN_ROOT / "compat" / "legacy_vision_scene_adapter.py"
VISION_IMPORT_ADAPTER = PLUGIN_ROOT / "compat" / "legacy_vision_import_adapter.py"


def test_scene_tools_has_a_local_boundary_document():
    boundary = PLUGIN_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    assert "scene.*" in source
    assert "sceneTools.*" in source
    assert "plugins/SceneTools/compat/legacy_vision_scene_adapter.py" in source
    assert "已删除 `legacy_vision_import_helper.py`" in source


def test_scene_tools_is_the_aggregate_owner_and_isolates_vision_legacy_access():
    main_source = MAIN_SOURCE.read_text(encoding="utf-8")
    adapter_source = VISION_ADAPTER.read_text(encoding="utf-8")
    import_adapter_source = VISION_IMPORT_ADAPTER.read_text(encoding="utf-8")

    assert "from api.editor_api import CoronaEditorApi" in main_source
    assert "@PluginBase.register_web(\"SceneTools\")" in main_source
    assert "from plugins.SceneTools.compat.legacy_vision_import_adapter import" in main_source
    assert "get_legacy_vision_scene" not in main_source
    assert "from runtime.legacy_scene_store import legacy_scene_store" in adapter_source
    assert "from plugins.SceneTools.compat.legacy_vision_scene_adapter import get_legacy_vision_scene" in import_adapter_source
    assert "scene_manager" not in main_source
    assert "CoronaCore" not in main_source
    assert "scene_manager" not in adapter_source


def test_runtime_vision_paths_remain_forwarding_aliases():
    from plugins.SceneTools.compat import legacy_vision_import_adapter as canonical_import
    from plugins.SceneTools.compat import legacy_vision_scene_adapter as canonical_scene
    from runtime import legacy_vision_import_adapter as import_alias
    from runtime import legacy_vision_scene_adapter as scene_alias

    assert import_alias.import_vision_scene_into_current_scene is canonical_import.import_vision_scene_into_current_scene
    assert scene_alias.get_legacy_vision_scene is canonical_scene.get_legacy_vision_scene


def test_unreferenced_vision_helper_has_been_removed():
    assert not (PLUGIN_ROOT / "compat" / "legacy_vision_import_helper.py").exists()
    assert not (PLUGIN_ROOT / "vision_import.py").exists()
