"""Contract for the single Web -> Python entry point.

C++ carries the envelope verbatim; routing and the service allowlist live on the
Python side.  These tests pin that split: JS owns the request schema, Python
owns the reachable services.

The C++ half is pinned too: `execute_bridge_operation` is a transport. It takes the
envelope string, hands it to the Python service untouched and returns what the service
said - no `json::parse`, no key inspection, no re-encoding. The two *other* interfaces
(`window.corona.*` for Web; the ScriptRuntime) are typed
on both sides; this one is deliberately not.
"""

import json
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
EDITOR_ROOT = ROOT / "embedded_python"
if str(EDITOR_ROOT) not in sys.path:
    sys.path.insert(0, str(EDITOR_ROOT))

from runtime.bridge_errors import BridgeError
from runtime.editor_host import CoronaEditor, WebBridgeService

BRIDGE_IMPL = ROOT / "src/systems/ui/cef/editor_service_operations.cpp"


def _function_body(source: str, signature: str) -> str:
    """The brace-balanced body of the function whose signature contains `signature`."""
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unbalanced body for {signature}")


def test_cpp_forwards_the_envelope_without_parsing_or_rebuilding_it():
    body = _function_body(BRIDGE_IMPL.read_text(encoding="utf-8-sig"),
                          "BridgeReply execute_bridge_operation")
    # The request leg is the envelope string, untouched. Only the reply leg converts the
    # service's own answer for JS, so the scan stops at the service call.
    request_leg = body[:body.index("invoke_python_script_service")]
    for forbidden in ("json::parse", "from_json", ".get<", '"service"', '"function"',
                      '"args"', '"op"'):
        assert forbidden not in request_leg, (
            f"C++ started interpreting the Web -> Python envelope ({forbidden}); "
            "the envelope schema belongs to JS and Python")
    assert '{"editor_bridge", "call", nlohmann::json::array({envelope})}' in body, (
        "the envelope must reach the Python service as a single untouched argument")


def test_cpp_only_rejects_an_empty_envelope():
    body = _function_body(BRIDGE_IMPL.read_text(encoding="utf-8-sig"),
                          "BridgeReply execute_bridge_operation")
    # The one check C++ is allowed: an empty string cannot be a request. Everything else
    # (shape, service, allowlist) is answered by Python.
    assert body.count("envelope.empty()") == 1


def test_the_v8_binding_posts_the_string_and_never_parses_it():
    binding = (ROOT / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8-sig")
    body = _function_body(binding, "invoke(BridgeMethod")
    assert "json::parse" not in body, "the V8 bridge parsed the envelope instead of forwarding it"
    assert "execute_bridge_operation(envelope)" in body


def _record(monkeypatch, calls, response):
    def fake(module_name, function_name, args):
        calls.append((module_name, function_name, args))
        return response

    monkeypatch.setattr(CoronaEditor, "dispatch_script_values_from_cpp", staticmethod(fake))


def test_bridge_routes_an_allowlisted_service(monkeypatch):
    calls = []
    _record(monkeypatch, calls, {"success": True, "data": {"ok": True}})
    result = WebBridgeService.call(
        json.dumps({"service": "project_files", "function": "load", "args": [{"path": "a"}]})
    )
    assert result == {"ok": True}
    assert calls == [("project_files", "load", [{"path": "a"}])]


def test_bridge_defaults_missing_args_to_an_empty_list(monkeypatch):
    calls = []
    _record(monkeypatch, calls, {"success": True, "data": None})
    assert WebBridgeService.call(json.dumps({"service": "project_files", "function": "load"})) is None
    assert calls == [("project_files", "load", [])]


def test_bridge_rejects_services_outside_the_web_allowlist():
    with pytest.raises(PermissionError):
        WebBridgeService.call(json.dumps({"service": "SomethingElse", "function": "call"}))


def test_bridge_rejects_malformed_envelopes():
    for envelope in [
        "not json",
        json.dumps([1, 2, 3]),
        json.dumps({"function": "load"}),
        json.dumps({"service": "project_files"}),
        json.dumps({"service": "project_files", "function": "load", "args": {"a": 1}}),
        123,
    ]:
        with pytest.raises((ValueError, TypeError)):
            WebBridgeService.call(envelope)


def test_bridge_surfaces_service_failures(monkeypatch):
    calls = []
    _record(monkeypatch, calls, {"success": False, "error": "boom"})
    with pytest.raises(RuntimeError, match="boom"):
        WebBridgeService.call(json.dumps({"service": "project_archive", "function": "open_scene"}))


def test_bridge_raises_a_status_error_refusal_with_its_code(monkeypatch):
    # A refusal is raised, not returned: Web branches on error.code in one try/catch.
    calls = []
    refusal = {"status": "error", "code": "PROJECT_CONTEXT_CHANGED",
               "message": "Project session is stale"}
    _record(monkeypatch, calls, {"success": True, "data": refusal})
    with pytest.raises(BridgeError) as raised:
        WebBridgeService.call(json.dumps({
            "service": "project_archive", "function": "file_manager",
            "args": [{"archive_session": "old", "project_path": "P:/w", "op": "delete_item", "args": [""]}]}))
    assert raised.value.code == "PROJECT_CONTEXT_CHANGED"
    assert raised.value.message == "Project session is stale"


def test_bridge_raises_a_success_false_document_refusal(monkeypatch):
    calls = []
    refusal = {"success": False, "error": "DOCUMENT_CONFLICT"}
    _record(monkeypatch, calls, {"success": True, "data": refusal})
    with pytest.raises(BridgeError) as raised:
        WebBridgeService.call(json.dumps({
            "service": "project_archive", "function": "commit_document", "args": [{}]}))
    assert raised.value.code == "DOCUMENT_CONFLICT"


def test_dispatcher_prefixes_a_bridge_error_with_its_code(monkeypatch):
    class Probe:
        @staticmethod
        def refuse(*_args):
            raise BridgeError("READ_ONLY_PROJECT", "Project is read-only")

    monkeypatch.setitem(CoronaEditor.module_list, "Probe", Probe)
    assert CoronaEditor.dispatch_script_values_from_cpp("Probe", "refuse", []) == {
        "success": False, "code": "READ_ONLY_PROJECT", "error": "Project is read-only"}


def test_register_script_dispatcher_exposes_the_bridge():
    CoronaEditor.module_list.pop("editor_bridge", None)
    CoronaEditor.register_script_dispatcher()
    assert CoronaEditor.module_list["editor_bridge"] is WebBridgeService
