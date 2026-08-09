import importlib.util
from pathlib import Path


MODULE_PATH = (
    Path(__file__).resolve().parents[1]
    / "cai_extensions"
    / "flows"
    / "model_retrieval_workflow"
    / "model_library_paths.py"
)
_spec = importlib.util.spec_from_file_location("model_library_paths", MODULE_PATH)
assert _spec and _spec.loader
model_library_paths = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(model_library_paths)


def test_model_library_writes_to_project_resource_root(tmp_path):
    root = model_library_paths.resolve_model_library_root(tmp_path, for_write=True)

    assert root == tmp_path / "Resource" / "local_model_library"


def test_existing_legacy_model_library_is_read_compatibility_fallback(tmp_path):
    legacy = tmp_path / "assets" / "local_model_library"
    legacy.mkdir(parents=True)

    root = model_library_paths.resolve_model_library_root(tmp_path)

    assert root == legacy


def test_writes_do_not_extend_legacy_model_library(tmp_path):
    legacy = tmp_path / "assets" / "local_model_library"
    legacy.mkdir(parents=True)

    root = model_library_paths.resolve_model_library_root(tmp_path, for_write=True)

    assert root == tmp_path / "Resource" / "local_model_library"
    assert not root.is_dir()
