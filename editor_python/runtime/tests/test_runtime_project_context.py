from types import SimpleNamespace
from unittest.mock import Mock

from api import editor_api
from runtime import project_context


def test_active_project_is_pulled_on_each_access(monkeypatch):
    monkeypatch.setattr(project_context, "_resolve_native_engine", lambda: SimpleNamespace(active_project_path="old"))
    read = Mock(side_effect=[{"archive_session": "A", "project_path": "first"}, {"archive_session": ""}])
    monkeypatch.setattr(editor_api, "get_script_runtime_editor_api", lambda: SimpleNamespace(scene=SimpleNamespace(read_active=read)))
    assert project_context.get_active_project_path() == "first"
    assert project_context.get_active_project_path() == ""
    assert read.call_count == 2
