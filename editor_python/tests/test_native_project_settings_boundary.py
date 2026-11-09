from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
HANDLERS = (
    REPO_ROOT
    / ".."
    / ".."
    / "src"
    / "systems"
    / "ui"
    / "cef"
    / "cef_editor_native_api_handlers.cpp"
).resolve()
PLUGIN = (
    REPO_ROOT
    / "plugins"
    / "ProjectSettings"
    / "main.py"
)
PYTHON_API = REPO_ROOT / "api" / "editor_api.py"


def test_project_settings_save_uses_scoped_python_archive():
    source = HANDLERS.read_text(encoding="utf-8")
    start = source.index("void register_project_settings_api_handlers")
    end = source.index("void register_scene_tools_api_handlers", start)
    section = source[start:end]

    assert '{"save_active_project_info", script_method}' not in section
    assert '{"save_active_project_info", []' in section
    assert "matches_archive_session(*scene, settings)" in section
    assert 'project_archive_call("update_metadata"' in section
    assert 'read_ini_file' not in section


def test_python_editor_api_has_explicit_project_settings_namespace():
    source = PYTHON_API.read_text(encoding="utf-8")
    assert "class _ProjectSettingsApi" in source
    assert 'project_settings = _ProjectSettingsApi()' in source
    assert '"project_settings.save_active_project_info"' in source
