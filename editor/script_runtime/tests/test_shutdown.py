import unittest
from unittest import mock

from script_runtime.blockly import main as blockly_main
from script_runtime.engine import corona_engine as corona_engine_scratch


class _NonCooperativeThread:
    name = "non-cooperative-scratch"

    def __init__(self):
        self.join_timeouts = []

    def is_alive(self):
        return True

    def join(self, timeout=None):
        self.join_timeouts.append(timeout)


class ScratchShutdownTests(unittest.TestCase):
    def test_service_shutdown_requests_all_contexts_and_reports_pending_threads(self):
        thread = _NonCooperativeThread()
        previous_exec = blockly_main.ScratchTool._exec_thread
        previous_context = blockly_main.ScratchTool._exec_context_id
        try:
            blockly_main.ScratchTool._exec_thread = thread
            blockly_main.ScratchTool._exec_context_id = "shutdown-context"
            with mock.patch.object(corona_engine_scratch, "request_stop_all") as request_all, \
                 mock.patch.object(corona_engine_scratch, "isolate_context") as isolate_context:
                blockly_main.ScratchTool.request_shutdown()
                snapshot = blockly_main.ScratchTool.shutdown()

            request_all.assert_called()
            isolate_context.assert_called()
            self.assertEqual(snapshot["state"], "stop_timeout")
            self.assertIn(thread.name, snapshot["pending_threads"])
        finally:
            blockly_main.ScratchTool._exec_thread = previous_exec
            blockly_main.ScratchTool._exec_context_id = previous_context

    def test_non_cooperative_thread_is_isolated_without_async_exception(self):
        thread = _NonCooperativeThread()
        with mock.patch.object(corona_engine_scratch, "request_stop") as request_stop, \
             mock.patch.object(corona_engine_scratch, "isolate_context") as isolate_context:
            stopped = blockly_main._request_thread_stop(
                thread,
                timeout=0.01,
                context_id="actor:scene:cube",
            )

        self.assertFalse(stopped)
        request_stop.assert_called_once_with("actor:scene:cube")
        isolate_context.assert_called_once()
        source = blockly_main.Path(blockly_main.__file__).read_text(encoding="utf-8")
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
        source = blockly_main.Path(corona_engine_scratch.__file__).read_text(encoding="utf-8")
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
