"""Services raise their refusals; nothing maps message text or payload shapes to a code.

`documents` used to answer ``{"success": False, "error": CODE}`` and `project_files`
matched exception messages against a prefix table. Both are gone: each site raises
``BridgeError(code, message)`` where it detects the problem, so the dispatcher and the
bridge only ever see one failure shape. Success values stay data and are never sniffed.
"""
import hashlib
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "embedded_python"))

from runtime.archive import documents, project_files, script_files  # noqa: E402
from runtime.bridge_errors import BridgeError  # noqa: E402
from runtime.registry import LazyPythonScriptService  # noqa: E402


@pytest.fixture
def project(tmp_path, monkeypatch):
    """Point each service's session guard at a temporary project root."""
    monkeypatch.setattr(documents, "require_project_session", lambda request: (tmp_path, None))
    monkeypatch.setattr(project_files, "require_project_session", lambda request: (tmp_path, None))
    monkeypatch.setattr(script_files, "require_project_session", lambda request: (tmp_path, None))
    return tmp_path


def test_a_document_route_outside_the_project_is_refused(project):
    with pytest.raises(BridgeError) as raised:
        documents.read_document({"document": "../outside.json"})

    assert raised.value.code == "INVALID_PROJECT_DOCUMENT"


def test_reserved_document_routes_are_refused(project):
    for route in ("assets.manifest.json", "Scripts/files/doc.json"):
        with pytest.raises(BridgeError) as raised:
            documents.read_document({"document": route})
        assert raised.value.code == "INVALID_PROJECT_DOCUMENT", route


def test_a_lost_document_compare_and_swap_is_refused(project):
    with pytest.raises(BridgeError) as raised:
        documents.commit_document({"document": "ui/doc.json", "content": {}, "version": "stale"})

    assert raised.value.code == "DOCUMENT_CONFLICT"


def test_a_document_read_keeps_its_success_shape(project):
    result = documents.read_document({"document": "ui/doc.json", "revision_only": True})

    assert result["success"] is True
    assert result["revision"] == hashlib.sha256(b"").hexdigest()
    assert result["content"] is None
    assert result["corrupt"] is False


def test_the_archive_root_cannot_be_deleted(project):
    with pytest.raises(BridgeError) as raised:
        project_files.file_manager({"op": "delete_item", "path": ""})

    assert raised.value.code == "PERMISSION_DENIED"


def test_an_unknown_file_operation_is_refused(project):
    with pytest.raises(BridgeError) as raised:
        project_files.file_manager({"op": "nope", "path": ""})

    assert raised.value.code == "INVALID_ARGUMENT"


def test_an_unsafe_file_name_is_refused(project):
    (project / "Actors").mkdir()

    with pytest.raises(BridgeError) as raised:
        project_files.file_manager({"op": "create_folder", "path": "Actors", "name": ".."})

    assert raised.value.code == "INVALID_ARGUMENT"


def test_a_rename_onto_an_existing_item_is_refused(project):
    (project / "a.txt").write_text("a", encoding="utf-8")
    (project / "b.txt").write_text("b", encoding="utf-8")

    with pytest.raises(BridgeError) as raised:
        project_files.file_manager({"op": "rename_item", "path": "a.txt", "name": "b.txt"})

    assert raised.value.code == "CONFLICT"


def test_a_missing_script_document_is_data_not_a_refusal(project):
    assert script_files.ProjectFiles.load({"key": "workspace"}) == {"status": "missing"}


def test_an_initializing_service_refuses_with_a_retryable_code(monkeypatch):
    service = LazyPythonScriptService("demo", "no_such_module", "Thing")
    monkeypatch.setattr(service, "start_background_load", lambda: None)

    with pytest.raises(BridgeError) as raised:
        service.load({})

    assert raised.value.code == "SERVICE_UNAVAILABLE"


def test_a_degraded_service_refuses_with_the_initialization_error(monkeypatch):
    service = LazyPythonScriptService("demo", "no_such_module", "Thing")
    monkeypatch.setattr(service, "start_background_load", lambda: None)
    monkeypatch.setattr(service, "_state", "degraded")
    monkeypatch.setattr(service, "_initialization_error", RuntimeError("boom"))

    with pytest.raises(BridgeError) as raised:
        service.load({})

    assert raised.value.code == "SERVICE_UNAVAILABLE"
    assert "boom" in raised.value.message
