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
FILE_MANAGER = REPO_ROOT / "plugins" / "FileManager" / "main.py"


def test_file_manager_has_native_handlers_instead_of_script_fallbacks():
    source = HANDLERS.read_text(encoding="utf-8")
    start = source.index("void register_file_manager_api_handlers")
    end = source.index("void register_project_launcher_api_handlers", start)
    handlers = source[start:end]

    for method in (
        "create_file", "create_folder", "delete_item", "get_file_tree",
        "get_files", "get_project_info", "open_file", "rename_item",
    ):
        assert f'{{"{method}", []' in handlers
        assert f'{{"{method}", script_method}}' not in handlers

    script_start = source.index("void register_python_script_api_handlers")
    script_source = source[script_start:]
    assert 'register_script_module("FileManager", file_manager_methods)' not in script_source


def test_native_file_manager_keeps_project_path_boundary_checks():
    source = HANDLERS.read_text(encoding="utf-8")
    assert "resolve_file_manager_project_path" in source
    assert "path_is_inside_project" in source
    assert "Unable to delete the project root" in source


def test_file_manager_python_is_reduced_to_public_adapter():
    source = FILE_MANAGER.read_text(encoding="utf-8")
    assert "CoronaEditorApi.files" in source
    assert "shutil.rmtree" not in source
    assert "os.remove(" not in source
    assert "legacy_file_scene_adapter" not in source
