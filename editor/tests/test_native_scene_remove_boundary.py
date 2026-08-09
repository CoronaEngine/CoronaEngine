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
MAIN_VIEW = (
    REPO_ROOT / "plugins" / "MainView" / "main.py"
)


def test_native_remove_scene_is_a_safe_main_view_handler():
    source = HANDLER.read_text(encoding="utf-8")
    start = source.index("void register_main_view_api_handlers")
    end = source.index("void register_project_settings_api_handlers", start)
    main_view = source[start:end]

    assert '{"remove_scene", script_method}' not in main_view
    assert '{"remove_scene", []' in main_view
    assert "path_is_inside_project" in main_view
    assert "std::filesystem::remove" in main_view
    assert "native_scene_request_matches" in main_view


def test_editor_api_exposes_native_scene_remove():
    source = EDITOR_API.read_text(encoding="utf-8")
    assert "def remove_scene(" in source
    assert '"main.remove_scene"' in source


def test_main_view_scene_removal_uses_native_lifecycle_contract():
    source = MAIN_VIEW.read_text(encoding="utf-8")
    start = source.index("    def remove_scene")
    end = source.index("    def switch_scene", start)
    remove_source = source[start:end]

    assert "CoronaEditorApi.main.remove_scene" in remove_source
    assert "os.remove(" not in remove_source
    assert "\n        remove_scene(scene_path)" not in remove_source
    assert "CoronaEditorApi.main.on_init" in remove_source
    assert "_write_project_scenes" not in remove_source
    assert "更新 project.ini" not in remove_source
