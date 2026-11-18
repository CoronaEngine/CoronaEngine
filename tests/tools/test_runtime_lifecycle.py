"""The host lifecycle is a small state machine; these pin its contract.

Transitions are idempotent, shutdown is terminal and its snapshot is reusable, and a
service call that arrives after shutdown is refused with the retryable code instead of
being flattened into "unknown service".
"""
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "embedded_python"))

from runtime import registry  # noqa: E402
from runtime.editor_host import CoronaEditor  # noqa: E402

_STATE = ("_runtime_state", "_shutdown_requested", "_runtime_initialized",
          "_runtime_started", "_shutdown_snapshot", "_last_runtime_warning",
          "_runtime_watchdog_error_logged", "_runtime_watchdog_file")


@pytest.fixture
def host(monkeypatch):
    """A fresh host: class state is a singleton, so each test saves and restores it."""
    saved_state = {name: getattr(CoronaEditor, name) for name in _STATE}
    saved_modules = dict(CoronaEditor.module_list)
    saved_engine = CoronaEditor.CoronaEngine
    monkeypatch.setattr(registry, "shutdown_python_script_services", lambda timeout=2.0: [])
    yield CoronaEditor
    for name, value in saved_state.items():
        setattr(CoronaEditor, name, value)
    CoronaEditor.CoronaEngine = saved_engine
    CoronaEditor.module_list.clear()
    CoronaEditor.module_list.update(saved_modules)


def test_initialize_is_idempotent(host):
    assert host.initialize_runtime() is True
    assert host.initialize_runtime() is True

    assert host._runtime_state == "initialized"
    assert host._runtime_initialized is True


def test_initialize_after_shutdown_is_refused(host):
    host.request_shutdown()

    assert host.initialize_runtime() is False


def test_start_records_running(host):
    assert host.start_runtime() is True

    assert host._runtime_state == "running"
    assert host._runtime_started is True


def test_shutdown_is_terminal_and_reuses_its_snapshot(host):
    host.start_runtime()

    first = host.shutdown_runtime()
    second = host.shutdown_runtime()

    assert first["runtime_state"] == "stopped"
    assert second is first
    assert host.start_runtime() is False
    assert host.initialize_runtime() is False


def test_a_call_after_shutdown_is_retryable(host):
    host.request_shutdown()

    result = host.dispatch_script_values_from_cpp("project_archive", "parse", [{}])

    assert result["success"] is False
    assert result["code"] == "SERVICE_UNAVAILABLE"


def test_registering_the_bridge_is_idempotent(host):
    calls = []

    class FakeEngine:
        def register_python_script_service_dispatcher(self):
            calls.append("register")

        def unregister_python_script_service_dispatcher(self):
            calls.append("unregister")

    host.CoronaEngine = FakeEngine()
    host.register_script_dispatcher()
    host.register_script_dispatcher()
    host.unregister_script_dispatcher()

    assert calls == ["register", "register", "unregister"]
    assert [name for name in host.module_list if name == "editor_bridge"] == ["editor_bridge"]


def test_lifecycle_transitions_are_logged_as_events(host, caplog):
    with caplog.at_level("INFO"):
        host.initialize_runtime()
        host.start_runtime()
        host.shutdown_runtime()

    for state in ("initializing", "initialized", "running", "stopping", "stopped"):
        assert f"event=runtime.lifecycle.state state={state}" in caplog.text, state
