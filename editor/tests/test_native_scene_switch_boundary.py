from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
HANDLER = (
    REPO_ROOT
    / ".."
    / "src"
    / "systems"
    / "ui"
    / "cef"
    / "cef_editor_native_api_handlers.cpp"
).resolve()
EDITOR_API = REPO_ROOT / "api" / "editor_api.py"
MAIN_VIEW = REPO_ROOT / "plugins" / "MainView" / "main.py"


def test_native_reload_scene_uses_the_requested_scene_route():
    source = HANDLER.read_text(encoding="utf-8")
    start = source.index("NativeEditorScene* reload_native_editor_scene")
    end = source.index("bool native_scene_request_matches", start)
    reload_source = source[start:end]

    assert "scene_route_arg" in reload_source
    assert "resolve_project_path" in reload_source
    assert "prepare_archive_load" in reload_source
    assert "(void)scene_route_arg" not in reload_source


def test_editor_api_exposes_native_scene_reload():
    source = EDITOR_API.read_text(encoding="utf-8")
    assert "def reload_scene(" in source
    assert '"scene_tools.reload_scene"' in source


def test_main_view_switch_uses_native_scene_reload():
    source = MAIN_VIEW.read_text(encoding="utf-8")
    start = source.index("    def switch_scene")
    end = source.index("    def scene_save", start)
    switch_source = source[start:end]

    assert "CoronaEditorApi.scene_tools.reload_scene" in switch_source
    assert "get_scene(" not in switch_source
    assert "get_or_create_scene(" not in switch_source
    assert "set_enabled(" not in switch_source
