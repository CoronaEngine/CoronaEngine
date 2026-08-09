from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
HANDLERS = (
    REPO_ROOT
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


def test_project_settings_save_is_owned_by_native_handler():
    source = HANDLERS.read_text(encoding="utf-8")
    start = source.index("void register_project_settings_api_handlers")
    end = source.index("void register_scene_tools_api_handlers", start)
    section = source[start:end]

    assert '{"save_active_project_info", script_method}' not in section
    assert '{"save_active_project_info", []' in section
    assert "detect_scene_folder" in section
    assert 'replace_ini_section_from_map(portable->scene_file, "scene"' in section
    assert "旧项目为只读" in section


def test_project_settings_python_entry_is_only_a_manifest_adapter():
    source = PLUGIN.read_text(encoding="utf-8")
    assert "CoronaEditorApi.project_settings.get_active_project_info" in source
    assert "CoronaEditorApi.project_settings.save_active_project_info" in source
    assert "settings_manager" not in source
    assert "configparser" not in source
    assert not (
        REPO_ROOT
        / "plugins"
        / "ProjectSettings"
        / "compat"
        / "legacy_project_settings.py"
    ).exists()


def test_python_editor_api_has_explicit_project_settings_namespace():
    source = PYTHON_API.read_text(encoding="utf-8")
    assert "class _ProjectSettingsApi" in source
    assert 'project_settings = _ProjectSettingsApi()' in source
    assert '"project_settings.save_active_project_info"' in source
