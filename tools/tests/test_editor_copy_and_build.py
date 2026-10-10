import importlib.util
from pathlib import Path
from unittest.mock import Mock

import pytest


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


def _prepare_frontend_build(module, tmp_path):
    frontend = tmp_path / "Frontend"
    node = tmp_path / "node"
    frontend.mkdir()
    node.mkdir()
    npm = node / ("npm.cmd" if module.os.name == "nt" else "npm")
    npm.write_text("mock npm executable\n", encoding="utf-8")
    return frontend, node, npm


@pytest.mark.parametrize("results", [[17], [0, 23]])
def test_editor_build_propagates_npm_failure_to_main(tmp_path, monkeypatch, results):
    module = _load_copy_module()
    frontend, node, npm = _prepare_frontend_build(module, tmp_path)
    # An older deployment must not make a failed rebuild appear successful.
    (frontend / "dist").mkdir()
    (frontend / "dist" / "index.html").write_text("old build", encoding="utf-8")
    runner = Mock(side_effect=results)
    monkeypatch.setattr(module, "stream_run", runner)

    result = module.main([
        "--dest-root", str(tmp_path / "CabbageEditor"),
        "--frontend-dir", str(frontend),
        "--node-dir", str(node),
    ])

    assert result == results[-1]
    assert runner.call_count == len(results)
    assert runner.call_args_list[0].args[0] == [str(npm), "install"]
    if len(results) == 2:
        assert runner.call_args_list[1].args[0] == [str(npm), "run", "build"]


@pytest.mark.parametrize("artifact", ["missing", "directory", "file"])
def test_editor_build_requires_html_output(tmp_path, monkeypatch, artifact):
    module = _load_copy_module()
    frontend, node, _ = _prepare_frontend_build(module, tmp_path)
    index = frontend / "dist" / "index.html"
    if artifact == "directory":
        index.mkdir(parents=True)
    elif artifact == "file":
        index.parent.mkdir()
        index.write_text("<!doctype html>", encoding="utf-8")
    runner = Mock(return_value=0)
    monkeypatch.setattr(module, "stream_run", runner)

    result = module.maybe_run_npm(frontend, node)

    assert result == (0 if artifact == "file" else 1)
    assert runner.call_count == 2


@pytest.mark.parametrize("invalid_kind", ["missing", "file"])
def test_editor_build_rejects_invalid_frontend_directory(tmp_path, monkeypatch, invalid_kind):
    module = _load_copy_module()
    frontend = tmp_path / "Frontend"
    if invalid_kind == "file":
        frontend.write_text("not a directory", encoding="utf-8")
    runner = Mock()
    monkeypatch.setattr(module, "stream_run", runner)

    assert module.maybe_run_npm(frontend, tmp_path / "node") != 0
    runner.assert_not_called()


@pytest.mark.parametrize("invalid_kind", ["missing", "directory"])
def test_editor_build_rejects_invalid_npm_executable(tmp_path, monkeypatch, invalid_kind):
    module = _load_copy_module()
    frontend, node, npm = _prepare_frontend_build(module, tmp_path)
    npm.unlink()
    if invalid_kind == "directory":
        npm.mkdir()
    runner = Mock()
    monkeypatch.setattr(module, "stream_run", runner)

    assert module.maybe_run_npm(frontend, node) != 0
    runner.assert_not_called()
