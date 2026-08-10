from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]
PLUGIN_ROOT = EDITOR_ROOT / "plugins" / "MainView"


def test_main_view_document_records_native_lifecycle_completion_for_repo_callers():
    source = (PLUGIN_ROOT / "main.py").read_text(encoding="utf-8")

    assert "CoronaEditorApi.main.on_init" in source
    assert "CoronaEditorApi.main.create_scene" in source
    assert "CoronaEditorApi.main.remove_scene" in source


def test_main_view_production_source_does_not_import_legacy_scene_adapter():
    source = (PLUGIN_ROOT / "main.py").read_text(encoding="utf-8")
    assert not (PLUGIN_ROOT / "compat" / "legacy_main_view.py").exists()
    assert not (PLUGIN_ROOT / "compat" / "legacy_main_view_scene_adapter.py").exists()

    assert "legacy_main_view_scene_adapter" not in source
    assert "CoronaEditorApi.main.on_init" in source
    assert "CoronaEditorApi.main.create_scene" in source
    assert "CoronaEditorApi.main.remove_scene" in source
    assert "CoronaEditorApi.scene_tools.reload_scene" in source
