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


def test_native_create_scene_is_not_a_python_script_fallback():
    source = HANDLER.read_text(encoding="utf-8")
    start = source.index('void register_scene_tools_api_handlers')
    scene_tools = source[start:]

    assert '{"create_scene", script_method}' not in scene_tools
    assert '{"create_scene", []' in scene_tools
    assert "ensure_native_editor_scene" in scene_tools
    assert "native_success" in scene_tools


def test_editor_api_exposes_the_native_create_scene_contract():
    source = EDITOR_API.read_text(encoding="utf-8")
    assert 'def create_scene(' in source
    assert '"scene_tools.create_scene"' in source
    assert '"main.create_scene"' in source


def test_main_view_scene_creation_does_not_construct_legacy_python_scene():
    source = MAIN_VIEW.read_text(encoding="utf-8")
    start = source.index("    def create_new_scene")
    end = source.index("    def remove_scene", start)
    create_source = source[start:end]

    assert "CoronaEditorApi.main.create_scene" in create_source
    assert "create_scene_from_template" not in create_source
    assert "get_or_create_scene" not in create_source
