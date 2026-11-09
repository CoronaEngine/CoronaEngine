import hashlib
from unittest.mock import patch

from script_runtime.file_runner import FileScripts


def test_run_uses_the_verified_file_even_if_another_panel_saves_again(tmp_path, monkeypatch):
    state = {"archive_session": "A", "project_path": str(tmp_path)}
    monkeypatch.setattr("runtime.project_context.read_project_session", lambda: state)
    path = tmp_path / "actor.py"
    original = b"def run(): return 1\n"
    path.write_bytes(original)
    request = {**state, "requestId": "initial", "path": "actor.py",
               "revision": hashlib.sha256(original).hexdigest(), "actor_name": "Player"}
    with patch.object(FileScripts, "_file_requests", {}), \
            patch.object(FileScripts, "_start_single", return_value={"runId": "run-1"}) as start:
        FileScripts.run_file(request)
        path.write_bytes(b"def run(): return 2\n")
        module = {}
        exec(start.call_args.kwargs["program"], module)
        assert module["run"]() == 1
        assert FileScripts.get_file_run(request)["runId"] == "run-1"
        assert FileScripts.run_file(request)["runId"] == "run-1"
        assert FileScripts.run_file(dict(request, actor_name="Other"))["code"] == "REQUEST_CONFLICT"
        assert FileScripts.run_file(dict(request, requestId="another"))["code"] == "SCRIPT_CHANGED"
        assert start.call_count == 1


def test_close_before_start_prevents_a_late_delivery(tmp_path, monkeypatch):
    state = {"archive_session": "A", "project_path": str(tmp_path)}
    monkeypatch.setattr("runtime.project_context.read_project_session", lambda: state)
    with patch.object(FileScripts, "_file_requests", {}), patch.object(FileScripts, "_start_single") as start:
        request = {**state, "requestId": "closed-panel"}
        assert FileScripts.get_file_run(request)["status"] == "unknown"
        assert FileScripts.stop_file(request)["status"] == "stopped"
        assert FileScripts.run_file(request)["status"] == "cancelled"
        start.assert_not_called()


def test_stale_stop_leaves_the_new_run_alone():
    with patch.object(FileScripts, "_exec_state", {"runId": "new"}), \
            patch.object(FileScripts, "stop_script_execution") as stop:
        assert FileScripts.stop_file({"runId": "old"})["code"] == "STALE_RUN"
        stop.assert_not_called()
        FileScripts.stop_file({"runId": "new"})
        stop.assert_called_once_with(True)
