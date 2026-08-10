from pathlib import Path


MAIN_VIEW_SOURCE = Path(__file__).resolve().parents[1] / "main.py"


def test_main_view_does_not_own_vision_scene_initialization():
    source = MAIN_VIEW_SOURCE.read_text(encoding="utf-8")

    assert "from api.editor_api import CoronaEditorApi" in source
    assert "CoronaEditor.CoronaEngine.is_vision_available" not in source
    assert "CoronaEditor.CoronaEngine.load_vision_scene" not in source
    assert "CoronaEditorApi.scene_tools.is_vision_available" not in source
    assert "CoronaEditorApi.scene_tools.load_vision_scene" not in source
    assert "CoronaEditorApi.main.on_init" in source


def test_main_view_does_not_import_editor_runtime_container_for_compat_event():
    source = MAIN_VIEW_SOURCE.read_text(encoding="utf-8")

    assert "from CoronaCore.core.corona_editor import CoronaEditor" not in source
    assert "from runtime.editor_host import emit_editor_event" in source


def test_main_view_scene_save_uses_native_aggregate_contract():
    source = MAIN_VIEW_SOURCE.read_text(encoding="utf-8")
    start = source.index("    def scene_save")
    end = source.index("    def run_project", start)
    save_source = source[start:end]

    assert "CoronaEditorApi.main.scene_save" in save_source
    assert "legacy_scene_store.get" not in save_source
    assert "scene.save_data" not in save_source


def test_main_view_run_project_uses_scene_snapshot_for_scene_lookup():
    source = MAIN_VIEW_SOURCE.read_text(encoding="utf-8")
    start = source.index("    def run_project")
    run_source = source[start:]

    assert "CoronaEditorApi.scene.get_snapshot" in run_source
    assert "legacy_scene_store.get" not in run_source
    assert "run_generated_script" in run_source
