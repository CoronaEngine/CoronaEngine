import importlib.util
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
COPY_SCRIPT = REPO_ROOT / "tools" / "build" / "editor_copy_and_build.py"


def _load_copy_module():
    spec = importlib.util.spec_from_file_location("editor_copy_and_build", COPY_SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_editor_copy_removes_stale_python_sources_from_deployed_editor(tmp_path):
    module = _load_copy_module()
    source = tmp_path / "editor"
    destination = tmp_path / "CabbageEditor"

    (source / "runtime").mkdir(parents=True)
    (source / "CoronaCore" / "core").mkdir(parents=True)
    (source / "runtime" / "bootstrap.py").write_text("CURRENT_BOOTSTRAP = True\n", encoding="utf-8")
    (source / "CoronaCore" / "core" / "corona_editor.py").write_text(
        "from runtime.editor_host import *\n", encoding="utf-8"
    )

    (destination / "runtime").mkdir(parents=True)
    (destination / "CoronaCore" / "core").mkdir(parents=True)
    (destination / "runtime" / "bootstrap.py").write_text("OLD_BOOTSTRAP = True\n", encoding="utf-8")
    stale = destination / "CoronaPlugin" / "utils" / "load_utils.py"
    stale.parent.mkdir(parents=True)
    stale.write_text("OLD_REGISTRY_IMPLEMENTATION = True\n", encoding="utf-8")

    module.copy_tree(source, destination, merge_content=True)

    assert (destination / "runtime" / "bootstrap.py").read_text(encoding="utf-8") == (
        "CURRENT_BOOTSTRAP = True\n"
    )
    assert not stale.exists()


def test_editor_copy_preserves_ignored_development_documents(tmp_path):
    module = _load_copy_module()
    source = tmp_path / "editor"
    destination = tmp_path / "CabbageEditor"

    source.mkdir()
    destination.mkdir()
    stale_doc = destination / "README.md"
    stale_doc.write_text("keep deployment note\n", encoding="utf-8")

    module.copy_tree(source, destination, merge_content=True)

    assert stale_doc.exists()


def test_editor_copy_preserves_deployed_project_and_generated_data(tmp_path):
    module = _load_copy_module()
    source = tmp_path / "editor"
    destination = tmp_path / "CabbageEditor"

    source.mkdir()
    destination.mkdir()
    project_script = destination / "data" / "world" / "Scripts" / "main.py"
    generated_script = destination / "runtime" / "generated" / "blockly_code.py"
    project_script.parent.mkdir(parents=True)
    generated_script.parent.mkdir(parents=True)
    project_script.write_text("project script\n", encoding="utf-8")
    generated_script.write_text("generated script\n", encoding="utf-8")

    module.copy_tree(source, destination, merge_content=True)

    assert project_script.exists()
    assert generated_script.exists()
