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
MANIFEST = (
    REPO_ROOT
    / ".."
    / "src"
    / "systems"
    / "ui"
    / "editor_api"
    / "cef_editor_api.cpp"
).resolve()
EDITOR_API = REPO_ROOT / "api" / "editor_api.py"
MAIN_VIEW = REPO_ROOT / "plugins" / "MainView" / "main.py"
FRONTEND_API = REPO_ROOT / "Frontend" / "src" / "api" / "editorApi.js"
FRONTEND_COMPAT = REPO_ROOT / "Frontend" / "src" / "compat" / "sceneService.js"


def test_main_scene_lifecycle_is_a_native_aggregate_contract():
    source = HANDLER.read_text(encoding="utf-8")
    start = source.index("void register_main_view_api_handlers")
    end = source.index("void register_project_settings_api_handlers", start)
    main_view = source[start:end]

    assert '{"create_scene", []' in main_view
    assert '{"remove_scene", []' in main_view
    assert "persist_native_project_scene_index" in main_view


def test_main_scene_lifecycle_is_registered_in_the_single_manifest():
    source = MANIFEST.read_text(encoding="utf-8")
    assert '"main.createScene", "main.create_scene"' in source
    assert '"main.removeScene", "main.remove_scene"' in source


def test_python_main_view_does_not_write_project_scene_index():
    source = MAIN_VIEW.read_text(encoding="utf-8")
    start = source.index("    def create_new_scene")
    end = source.index("    def switch_scene", start)
    lifecycle = source[start:end]

    assert "CoronaEditorApi.main.create_scene" in lifecycle
    assert "CoronaEditorApi.main.remove_scene" in lifecycle
    assert "_write_project_scenes" not in lifecycle
    assert "get_project_scenes" not in lifecycle


def test_editor_api_exposes_main_scene_lifecycle():
    source = EDITOR_API.read_text(encoding="utf-8")
    assert '"main.create_scene"' in source
    assert '"main.remove_scene"' in source


def test_vue_scene_creation_uses_the_main_lifecycle_adapter():
    api_source = FRONTEND_API.read_text(encoding="utf-8")
    compat_source = FRONTEND_COMPAT.read_text(encoding="utf-8")
    assert "createScene: (sceneName) =>" in api_source
    assert "call_manifest_editor_api('main.createScene'" in api_source
    assert "editorApi.main.createScene(sceneName)" in compat_source
