"""Blockly requests must belong to the current opening of the project."""
from unittest.mock import Mock

import pytest
from runtime import project_context
from script_runtime.file_runner import FileScripts
from runtime.archive.script_files import ProjectFiles


def test_reopening_same_project_rejects_old_session(tmp_path, monkeypatch):
    current = {"archive_session": "first-A", "project_path": str(tmp_path)}
    monkeypatch.setattr(project_context, "read_project_session", lambda: dict(current))
    request = {**current, "requestId": "old", "path": "script.py"}
    assert project_context.require_project_session(request) == (tmp_path, None)
    assert ProjectFiles.load({**request, "key": "project"})["status"] == "missing"
    assert not (tmp_path / "Scripts").exists()
    current["archive_session"] = "second-A"
    start = Mock()
    monkeypatch.setattr(FileScripts, "_start_single", start)
    for method in (ProjectFiles.load, ProjectFiles.save,
                   FileScripts.run_file):
        assert method(request)["code"] == "PROJECT_CONTEXT_CHANGED"
        assert method({**request, "archive_session": ""})["code"] == "PROJECT_CONTEXT_CHANGED"
    start.assert_not_called()
    assert not (tmp_path / "Scripts").exists()


@pytest.mark.parametrize("state,code", [({}, "NO_ACTIVE_PROJECT"),
    ({"archive_session": "A", "read_only": True}, "READ_ONLY_PROJECT")])
def test_unavailable_or_read_only_project_is_not_writable(tmp_path, monkeypatch, state, code):
    monkeypatch.setattr(project_context, "read_project_session", lambda: {**state, "project_path": str(tmp_path)})
    request = {"project_path": str(tmp_path), "archive_session": "A"}
    assert ProjectFiles.save(request)["code"] == code
    assert not (tmp_path / "Scripts").exists()
