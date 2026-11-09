import unittest
from unittest import mock

from pathlib import Path
from script_runtime import file_api, file_runner
from Frontend.src.blockly.generators.runtime import block_program as corona_engine_scratch


class _NonCooperativeThread:
    name = "non-cooperative-scratch"

    def __init__(self):
        self.join_timeouts = []

    def is_alive(self):
        return True

    def join(self, timeout=None):
        self.join_timeouts.append(timeout)


class ScratchShutdownTests(unittest.TestCase):
    def test_service_shutdown_isolates_non_cooperative_threads(self):
        thread = _NonCooperativeThread()
        run = file_api.FileRun("shutdown-context", "session", "actor", "scene.ini", "Probe")
        run.on_stop = mock.Mock()
        run.on_quarantine = mock.Mock()
        with mock.patch.multiple(file_runner.FileScripts, _exec_thread=thread, _run=run,
                                 _shutdown_requested=False), mock.patch.object(file_runner, "set_editor_camera_input_enabled"):
            snapshot = file_runner.FileScripts.shutdown(timeout=0.01)
        self.assertEqual(snapshot["state"], "stop_timeout")
        self.assertIn(thread.name, snapshot["pending_threads"])
        run.on_stop.assert_called_once_with()
        run.on_quarantine.assert_called_once()
        with file_api.bind(run), self.assertRaises(SystemExit):
            file_api.check_stop()

    def test_shutdown_is_cooperative(self):
        source = Path(file_runner.__file__).read_text(encoding="utf-8")
        self.assertNotIn("PyThreadState_SetAsyncExc", source)
        self.assertNotIn("import ctypes", source)

    def test_isolated_context_rejects_later_engine_operations(self):
        context = corona_engine_scratch.create_context("isolated-test")
        try:
            corona_engine_scratch.isolate_context("isolated-test", "shutdown timeout")
            with corona_engine_scratch.using_context(context):
                with self.assertRaises(SystemExit):
                    corona_engine_scratch.assert_engine_operation_allowed()
        finally:
            corona_engine_scratch.release_context(context)

    def test_handler_and_broadcast_boundaries_include_cooperative_checkpoints(self):
        source = Path(corona_engine_scratch.__file__).read_text(encoding="utf-8")
        handler = source[
            source.index("def _run_bound_handler"):
            source.index("def _run_tracked_handler")
        ]
        broadcast = source[
            source.index("def _broadcast"):
            source.index("def broadcast(message)")
        ]

        self.assertIn("check_stop()", handler)
        self.assertIn("check_stop()", broadcast)
        self.assertIn("thread.join(timeout=", broadcast)
        self.assertNotIn("thread.join()", broadcast)


if __name__ == "__main__":
    unittest.main()
