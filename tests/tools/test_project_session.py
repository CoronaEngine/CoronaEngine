"""The session guard every Python project service runs before touching files.

`read_project_session` pulls the identity from the native module on each call:
`archive_session` is minted by the parser but owned by the C++ scene afterwards, so a
cached copy would go stale the moment the editor reopens the project.

`archive_session` answers "is this still the project instance the caller loaded"; the
path alone cannot, because reopening the same folder mints a new session. `read_only`
turns the same guard into the 403 path. These tests pin all four answers plus the
fail-closed behaviour of a native module that predates the session reader.
"""
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "embedded_python"))

from runtime import project_context  # noqa: E402


class _Engine:
    """Stand-in for the native module: only the readers it is configured with exist."""

    def __init__(self, session=None, path=None):
        if session is not None:
            self.active_project_session = lambda: dict(session)
        if path is not None:
            self.active_project_path = lambda: path


@pytest.fixture
def engine(monkeypatch):
    def install(instance):
        monkeypatch.setattr(project_context, "_resolve_native_engine", lambda: instance)
        monkeypatch.setattr(project_context, "_path_only_binding_warned", False)

    return install


def test_no_native_module_means_no_session(engine):
    engine(None)

    assert project_context.read_project_session() == {}
    assert project_context.require_project_session({}) == (
        None,
        {"status": "error", "code": "NO_ACTIVE_PROJECT", "message": "No project is open"},
    )


def test_a_matching_session_is_accepted(engine, tmp_path):
    engine(_Engine(session={
        "project_path": str(tmp_path), "archive_session": "abc", "read_only": False}))

    root, error = project_context.require_project_session(
        {"project_path": str(tmp_path), "archive_session": "abc"})

    assert error is None
    assert root == tmp_path.resolve()


def test_a_stale_archive_session_is_refused(engine, tmp_path):
    engine(_Engine(session={"project_path": str(tmp_path), "archive_session": "new"}))

    _, error = project_context.require_project_session(
        {"project_path": str(tmp_path), "archive_session": "old"})

    assert error["code"] == "PROJECT_CONTEXT_CHANGED"


def test_a_replaced_project_path_is_refused(engine, tmp_path):
    engine(_Engine(session={"project_path": str(tmp_path), "archive_session": "abc"}))

    _, error = project_context.require_project_session(
        {"project_path": str(tmp_path / "other"), "archive_session": "abc"})

    assert error["code"] == "PROJECT_CONTEXT_CHANGED"


def test_a_read_only_project_refuses_writes(engine, tmp_path):
    engine(_Engine(session={
        "project_path": str(tmp_path), "archive_session": "abc", "read_only": True}))

    _, error = project_context.require_project_session(
        {"project_path": str(tmp_path), "archive_session": "abc"})

    assert error["code"] == "READ_ONLY_PROJECT"


def test_a_path_only_binding_fails_closed_and_says_so(engine, tmp_path, caplog):
    # An older native module has no session reader: callers must be refused, and the
    # missing binding has to be visible instead of looking like "no project is open".
    engine(_Engine(path=str(tmp_path)))

    with caplog.at_level("WARNING"):
        session = project_context.read_project_session()
        once_more = project_context.read_project_session()
        _, error = project_context.require_project_session(
            {"project_path": str(tmp_path), "archive_session": "abc"})

    assert session == {"project_path": str(tmp_path)}
    assert once_more == session
    assert error["code"] == "NO_ACTIVE_PROJECT"
    assert caplog.text.count("runtime.session.binding_missing") == 1
