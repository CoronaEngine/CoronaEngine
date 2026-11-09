from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_project_lifecycle_manifest_methods_have_public_wrappers():
    source = (REPO_ROOT / "src/systems/ui/editor_api/cef_editor_api.cpp").read_text(
        encoding="utf-8"
    )
    expected = {
        "ProjectLauncher.get_app_version": ("project.getAppVersion", "project.get_app_version"),
        "ProjectLauncher.browse_folder": ("project.browseFolder", "project.browse_folder"),
        "ProjectLauncher.create_multiplayer_project": ("project.createMultiplayerProject", "project.create_multiplayer_project"),
        "ProjectLauncher.create_project": ("project.createProject", "project.create_project"),
        "ProjectLauncher.create_world_project": ("project.createWorldProject", "project.create_world_project"),
        "ProjectLauncher.get_default_project_path": ("project.getDefaultProjectPath", "project.get_default_project_path"),
        "ProjectLauncher.get_project_load_status": ("project.getProjectLoadStatus", "project.get_project_load_status"),
        "ProjectLauncher.get_recent_projects": ("project.getRecentProjects", "project.get_recent_projects"),
        "ProjectLauncher.open_project": ("project.openProject", "project.open_project"),
        "ProjectLauncher.open_project_file": ("project.openProjectFile", "project.open_project_file"),
        "ProjectSettings.get_active_project_info": ("projectSettings.getActiveProjectInfo", "project_settings.get_active_project_info"),
        "ProjectSettings.save_active_project_info": ("projectSettings.saveActiveProjectInfo", "project_settings.save_active_project_info"),
        "MainView.on_init": ("main.onInit", "main.on_init"),
    }
    retired_web = {
        'ProjectLauncher.browse_folder', 'ProjectLauncher.create_project',
        'ProjectLauncher.get_default_project_path', 'ProjectSettings.get_active_project_info',
        'ProjectLauncher.get_app_version', 'ProjectLauncher.get_recent_projects',
        'ProjectLauncher.get_project_load_status',
        'ProjectLauncher.create_world_project', 'ProjectLauncher.create_multiplayer_project',
        'ProjectLauncher.open_project_file', 'ProjectLauncher.open_project',
        'ProjectSettings.save_active_project_info', 'MainView.on_init',
    }
    for api_name, (js_wrapper, python_wrapper) in expected.items():
        module, method = api_name.split(".", 1)
        line = next(line for line in source.splitlines() if f'({module}, {method},' in line)
        assert f'"{python_wrapper}"' in line, api_name
        if api_name in retired_web:
            assert ', "", ' in line, api_name
            assert ('~caller_mask(EditorApiCaller::Cef)' in line
                    or line.endswith('caller_mask(EditorApiCaller::PythonScript)),')), api_name
        else:
            assert f'"{js_wrapper}"' in line, api_name


def test_active_project_info_exposes_goal_metadata_for_editor_adapters():
    source = (
        REPO_ROOT / "editor_python/runtime/archive/project.py"
    ).read_text(encoding="utf-8")

    assert '"prompt": config.get("world", "prompt"' in source
    assert '"mode": config.get("world", "type", fallback="creative")' in source
