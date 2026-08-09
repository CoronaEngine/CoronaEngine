from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
PLUGIN = REPO_ROOT / "plugins" / "ProjectLauncher" / "main.py"
EDITOR_API = REPO_ROOT / "api" / "editor_api.py"


def test_project_launcher_entrypoint_delegates_project_operations_to_manifest():
    source = PLUGIN.read_text(encoding="utf-8")
    assert "from config.settings import settings_manager" not in source
    assert "CoronaEditorApi.project.get_default_project_path" in source
    assert "CoronaEditorApi.project.get_recent_projects" in source
    assert "CoronaEditorApi.project.create_project" in source
    assert "CoronaEditorApi.project.open_project" in source
    assert "return \"\"" not in source
    assert "return {}" not in source


def test_historical_project_launcher_service_shim_is_removed_after_owner_migration():
    assert not (
        REPO_ROOT
        / "plugins"
        / "ProjectLauncher"
        / "compat"
        / "legacy_project_launcher.py"
    ).exists()


def test_python_project_adapter_exposes_native_project_operations():
    source = EDITOR_API.read_text(encoding="utf-8")
    for method in (
        "get_default_project_path",
        "get_recent_projects",
        "create_project",
        "create_world_project",
        "create_multiplayer_project",
        "copy_existing_to_data",
        "open_project",
        "set_project_mode",
    ):
        assert f'"project.{method}"' in source


def test_native_project_copy_owns_legacy_copy_semantics():
    manifest = (
        REPO_ROOT.parent
        / "src"
        / "systems"
        / "ui"
        / "editor_api"
        / "cef_editor_api.cpp"
    ).read_text(encoding="utf-8")
    handlers = (
        REPO_ROOT.parent
        / "src"
        / "systems"
        / "ui"
        / "cef"
        / "cef_editor_native_api_handlers.cpp"
    ).read_text(encoding="utf-8")

    assert '"project.copyExistingToData"' in manifest
    assert '"project.copy_existing_to_data"' in manifest
    for marker in (
        "copy_existing_project_to_data_native",
        'payload.value(\"sourcePath\"',
        'payload.value(\"dataRoot\"',
        "std::filesystem::copy_options::recursive",
        "normalize_project_runtime_paths_native(target)",
        "project_values[\"name\"]",
        "std::filesystem::remove_all(target, ec)",
    ):
        assert marker in handlers
