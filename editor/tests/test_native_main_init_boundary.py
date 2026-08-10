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


def test_native_main_init_owns_scene_list_and_active_scene_resolution():
    source = HANDLER.read_text(encoding="utf-8")
    start = source.index("void register_main_view_api_handlers")
    end = source.index("void register_project_settings_api_handlers", start)
    main_view = source[start:end]

    assert '{"on_init", script_method}' not in main_view
    assert '{"on_init", []' in main_view
    assert "make_native_main_init_payload" in main_view
    assert "read_ini_file" in source
    assert "split_csv_routes" in source
    assert "reload_native_editor_scene" in source


def test_editor_api_exposes_native_main_init():
    source = EDITOR_API.read_text(encoding="utf-8")
    assert "def on_init(" in source
    assert '"main.on_init"' in source


def test_main_view_on_init_does_not_create_legacy_python_scenes():
    source = MAIN_VIEW.read_text(encoding="utf-8")
    start = source.index("    def on_init")
    end = source.index("    def create_new_scene", start)
    init_source = source[start:end]

    assert "CoronaEditorApi.main.on_init" in init_source
    assert "get_or_create_scene(" not in init_source
    assert "get_scene(" not in init_source
    assert "set_enabled(" not in init_source
