from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_file_manager_manifest_methods_have_the_files_adapter_wrappers():
    source = (REPO_ROOT / "src/systems/ui/editor_api/cef_editor_api.cpp").read_text(
        encoding="utf-8"
    )
    expected = {
        "create_file": ("files.createFile", "files.create_file"),
        "create_folder": ("files.createFolder", "files.create_folder"),
        "delete_item": ("files.deleteItem", "files.delete_item"),
        "get_file_tree": ("files.getFileTree", "files.get_file_tree"),
        "get_files": ("files.getFiles", "files.get_files"),
        "get_project_info": ("files.getProjectInfo", "files.get_project_info"),
        "open_file": ("files.openFile", "files.open_file"),
        "rename_item": ("files.renameItem", "files.rename_item"),
    }
    for method, (js_wrapper, python_wrapper) in expected.items():
        assert (
            f'EDITOR_API_METHOD_SCHEMA_WRAPPED(FileManager, {method},' in source
            and f'"{js_wrapper}"' in source
            and f'"{python_wrapper}"' in source
        ), method
