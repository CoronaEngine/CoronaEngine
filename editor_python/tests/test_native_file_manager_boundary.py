from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]
ENGINE_ROOT = EDITOR_ROOT.parent.parent
HANDLERS = (
    ENGINE_ROOT
    / "src"
    / "systems"
    / "ui"
    / "cef"
    / "cef_editor_native_api_handlers.cpp"
).resolve()
FILE_MANAGER = (
    EDITOR_ROOT / "plugins" / "FileManager" / "main.py"
)


def test_file_manager_has_native_handlers_instead_of_script_fallbacks():
    source = HANDLERS.read_text(encoding="utf-8")
    start = source.index("void register_file_manager_api_handlers")
    end = source.index("void register_project_launcher_api_handlers", start)
    handlers = source[start:end]

    for method in (
        "create_file", "create_folder", "delete_item", "get_file_tree",
        "get_files", "get_project_info", "open_file", "rename_item",
    ):
        assert f'{{"{method}",' in handlers
        assert f'{{"{method}", script_method}}' not in handlers

    script_start = source.index("void register_python_script_api_handlers")
    script_source = source[script_start:]
    assert 'register_script_module("FileManager", file_manager_methods)' not in script_source


def test_native_file_manager_keeps_project_path_boundary_checks():
    source = HANDLERS.read_text(encoding="utf-8")
    assert "resolve_file_manager_project_path" in source
    assert "path_is_inside_project" in source
    owner = (EDITOR_ROOT / "runtime/archive/project_files.py").read_text(encoding="utf8")
    assert 'project_archive_call("file_manager"' in source
    assert "require_project_session(request)" in owner
    assert "if not path.is_relative_to(root)" in owner and "if path == root or" in owner
