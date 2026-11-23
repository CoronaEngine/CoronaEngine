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


def test_bridge_reports_a_codeless_failure_as_service_failed(monkeypatch):
    calls = []
    _record(monkeypatch, calls, {"success": False, "error": "boom"})
    with pytest.raises(BridgeError) as raised:
        WebBridgeService.call(json.dumps({"service": "project_archive", "function": "open_scene"}))
    assert raised.value.code == "SERVICE_FAILED"
    assert raised.value.message == "boom"


def test_bridge_uses_the_dispatcher_code_and_never_parses_the_message(monkeypatch):
    # A message that happens to look like "CODE: text" must not become the code.
    calls = []
    _record(monkeypatch, calls, {"success": False, "code": "READ_ONLY_PROJECT",
                                 "error": "NOT_A_CODE: text stays text"})
    with pytest.raises(BridgeError) as raised:
        WebBridgeService.call(json.dumps({
            "service": "project_archive", "function": "save", "args": [{}]}))
    assert raised.value.code == "READ_ONLY_PROJECT"
    assert raised.value.message == "NOT_A_CODE: text stays text"


def test_bridge_returns_the_service_answer_verbatim(monkeypatch):
    # Success values are data. A payload that carries "status" or "success" keys is still
    # data: services raise refusals, so the bridge no longer sniffs shapes.
    answer = {"status": "success", "path": "Actors/a.actor", "success": True}
    _record(monkeypatch, [], {"success": True, "data": answer})
    assert WebBridgeService.call(json.dumps({
        "service": "project_archive", "function": "file_manager", "args": [{}]})) == answer


def test_bridge_refuses_when_the_dispatcher_reports_a_code(monkeypatch):
    # The dispatcher envelope is the only failure channel: code + message, no body shapes.
    _record(monkeypatch, [], {"success": False, "code": "PROJECT_CONTEXT_CHANGED",
                              "error": "Project session is stale"})
    with pytest.raises(BridgeError) as raised:
        WebBridgeService.call(json.dumps({
            "service": "project_archive", "function": "file_manager", "args": [{}]}))
    assert raised.value.code == "PROJECT_CONTEXT_CHANGED"
    assert raised.value.message == "Project session is stale"


def test_dispatcher_keeps_a_bridge_error_code(monkeypatch):
    class Probe:
        @staticmethod
        def refuse(*_args):
            raise BridgeError("READ_ONLY_PROJECT", "Project is read-only")

    monkeypatch.setitem(CoronaEditor.module_list, "Probe", Probe)
    assert CoronaEditor.dispatch_script_values_from_cpp("Probe", "refuse", []) == {
        "success": False, "code": "READ_ONLY_PROJECT", "error": "Project is read-only"}


def test_dispatcher_codes_an_unknown_service(monkeypatch):
    monkeypatch.delitem(CoronaEditor.module_list, "Nope", raising=False)
    assert CoronaEditor.dispatch_script_values_from_cpp("Nope", "call", []) == {
        "success": False, "code": "INVALID_ARGUMENT",
        "error": "Unknown Python service: Nope"}


def test_dispatcher_codes_an_unknown_function(monkeypatch):
    class Probe:
        pass

    monkeypatch.setitem(CoronaEditor.module_list, "Probe", Probe)
    result = CoronaEditor.dispatch_script_values_from_cpp("Probe", "missing", [])
    assert result["success"] is False
    assert result["code"] == "INVALID_ARGUMENT"


def test_dispatcher_codes_an_uncaught_exception_without_prefixing_the_message(monkeypatch):
    class Probe:
        @staticmethod
        def explode(*_args):
            raise RuntimeError("kaboom")

    monkeypatch.setitem(CoronaEditor.module_list, "Probe", Probe)
    assert CoronaEditor.dispatch_script_values_from_cpp("Probe", "explode", []) == {
        "success": False, "code": "SERVICE_FAILED", "error": "kaboom"}


def test_register_script_dispatcher_exposes_the_bridge():
    CoronaEditor.module_list.pop("editor_bridge", None)
    CoronaEditor.register_script_dispatcher()
    assert CoronaEditor.module_list["editor_bridge"] is WebBridgeService
