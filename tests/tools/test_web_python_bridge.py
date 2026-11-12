"""Contract for the single Web -> Python entry point.

C++ carries the envelope verbatim; routing and the service allowlist live on the
Python side.  These tests pin that split: JS owns the request schema, Python
owns the reachable services.
"""

import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
EDITOR_ROOT = ROOT / "embedded_python"
if str(EDITOR_ROOT) not in sys.path:
    sys.path.insert(0, str(EDITOR_ROOT))

from runtime.editor_host import CoronaEditor, WebBridgeService


def _record(monkeypatch, calls, response):
    def fake(module_name, function_name, args):
        calls.append((module_name, function_name, args))
        return response

    monkeypatch.setattr(CoronaEditor, "dispatch_script_values_from_cpp", staticmethod(fake))


def test_bridge_routes_an_allowlisted_service(monkeypatch):
    calls = []
    _record(monkeypatch, calls, {"success": True, "data": {"ok": True}})
    result = WebBridgeService.call(
        json.dumps({"service": "ProjectFiles", "function": "load", "args": [{"path": "a"}]})
    )
    assert result == {"ok": True}
    assert calls == [("ProjectFiles", "load", [{"path": "a"}])]


def test_bridge_defaults_missing_args_to_an_empty_list(monkeypatch):
    calls = []
    _record(monkeypatch, calls, {"success": True, "data": None})
    assert WebBridgeService.call(json.dumps({"service": "FileScripts", "function": "get_status"})) is None
    assert calls == [("FileScripts", "get_status", [])]


def test_bridge_rejects_services_outside_the_web_allowlist():
    with pytest.raises(PermissionError):
        WebBridgeService.call(json.dumps({"service": "SomethingElse", "function": "call"}))


def test_bridge_rejects_malformed_envelopes():
    for envelope in [
        "not json",
        json.dumps([1, 2, 3]),
        json.dumps({"function": "load"}),
        json.dumps({"service": "ProjectFiles"}),
        json.dumps({"service": "ProjectFiles", "function": "load", "args": {"a": 1}}),
        123,
    ]:
        with pytest.raises((ValueError, TypeError)):
            WebBridgeService.call(envelope)


def test_bridge_surfaces_service_failures(monkeypatch):
    calls = []
    _record(monkeypatch, calls, {"success": False, "error": "boom"})
    with pytest.raises(RuntimeError, match="boom"):
        WebBridgeService.call(json.dumps({"service": "ProjectArchive", "function": "open_scene"}))


def test_bridge_passes_a_structured_refusal_through_unchanged(monkeypatch):
    # A service that refuses (stale archive, read-only project, bad path) answers with
    # {status:'error', code} instead of raising, so Web can branch on the code. Only a
    # dispatcher-level failure becomes SERVICE_FAILED on the C++ side.
    calls = []
    refusal = {"status": "error", "code": "PROJECT_CONTEXT_CHANGED",
               "message": "Project session is stale"}
    _record(monkeypatch, calls, {"success": True, "data": refusal})
    result = WebBridgeService.call(json.dumps({
        "service": "ProjectArchive", "function": "file_manager",
        "args": [{"archive_session": "old", "project_path": "P:/w", "op": "delete_item", "args": [""]}]}))
    assert result == refusal


def test_register_script_dispatcher_exposes_the_bridge():
    CoronaEditor.module_list.pop("EditorBridge", None)
    CoronaEditor.register_script_dispatcher()
    assert CoronaEditor.module_list["EditorBridge"] is WebBridgeService
