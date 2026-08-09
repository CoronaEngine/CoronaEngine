from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_project_lifecycle_manifest_methods_have_public_wrappers():
    source = (REPO_ROOT / "src/systems/ui/editor_api/cef_editor_api.cpp").read_text(
        encoding="utf-8"
    )
    expected = {
        "ProjectLauncher.browse_folder": ("project.browseFolder", "project.browse_folder"),
        "ProjectLauncher.create_multiplayer_project": ("project.createMultiplayerProject", "project.create_multiplayer_project"),
        "ProjectLauncher.create_project": ("project.createProject", "project.create_project"),
        "ProjectLauncher.create_world_project": ("project.createWorldProject", "project.create_world_project"),
        "ProjectLauncher.get_default_project_path": ("project.getDefaultProjectPath", "project.get_default_project_path"),
        "ProjectLauncher.get_project_load_status": ("project.getProjectLoadStatus", "project.get_project_load_status"),
        "ProjectLauncher.get_recent_projects": ("project.getRecentProjects", "project.get_recent_projects"),
        "ProjectLauncher.open_project": ("project.openProject", "project.open_project"),
        "ProjectLauncher.open_project_file": ("project.openProjectFile", "project.open_project_file"),
        "ProjectLauncher.set_project_mode": ("project.setProjectMode", "project.set_project_mode"),
        "ProjectSettings.browse_scene_file": ("projectSettings.browseSceneFile", "project_settings.browse_scene_file"),
        "ProjectSettings.get_active_project_info": ("projectSettings.getActiveProjectInfo", "project_settings.get_active_project_info"),
        "ProjectSettings.save_active_project_info": ("projectSettings.saveActiveProjectInfo", "project_settings.save_active_project_info"),
        "MainView.get_menu_data": ("main.getMenuData", "main.get_menu_data"),
        "MainView.import_resource_file": ("main.importResourceFile", "main.import_resource_file"),
        "MainView.on_init": ("main.onInit", "main.on_init"),
        "MainView.create_scene": ("main.createScene", "main.create_scene"),
        "MainView.remove_scene": ("main.removeScene", "main.remove_scene"),
        "MainView.run_project": ("main.runProject", "main.run_project"),
        "MainView.update_view_tool_state": ("main.updateViewToolState", "main.update_view_tool_state"),
    }
    for api_name, (js_wrapper, python_wrapper) in expected.items():
        module, method = api_name.split(".", 1)
        assert (
            f'EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method},' in source
            and f'"{js_wrapper}"' in source
            and f'"{python_wrapper}"' in source
        ), api_name


def test_active_project_info_exposes_goal_metadata_for_editor_adapters():
    source = (
        REPO_ROOT / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp"
    ).read_text(encoding="utf-8")

    assert '"prompt", ini_value(scene_ini, "world", "prompt")' in source
    assert '"mode", ini_value(scene_ini, "world", "type", "story")' in source
    assert '"prompt", value("world_prompt", value("prompt", ""))' in source
