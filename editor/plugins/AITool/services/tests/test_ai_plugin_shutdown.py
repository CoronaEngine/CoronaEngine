import unittest
import threading
from types import SimpleNamespace
from unittest.mock import patch

from editor.plugins.AITool.services.ai_plugin_controller import AIPluginController
from editor.plugins.AITool.Quasar.ai_tools import warmup


class AIPluginShutdownTests(unittest.TestCase):
    def test_parallel_warmup_stops_waiting_when_stop_is_requested(self):
        stop = threading.Event()
        task_started = threading.Event()
        release_task = threading.Event()
        warmup_finished = threading.Event()

        def blocking_task():
            task_started.set()
            release_task.wait(2.0)

        with (
            patch.object(warmup, "warmup_configs", return_value=None),
            patch.object(warmup, "warmup_storage", side_effect=blocking_task),
            patch.object(warmup, "warmup_http_clients", return_value=None),
            patch.object(warmup, "warmup_account_pools", return_value=None),
            patch.object(warmup, "warmup_tools", return_value=None),
            patch.object(warmup, "warmup_workflows", return_value=None),
            patch.object(warmup, "warmup_agent", return_value=None),
        ):
            worker = threading.Thread(
                target=lambda: (
                    warmup.warmup_all(stop_token=stop),
                    warmup_finished.set(),
                )
            )
            worker.start()
            self.assertTrue(task_started.wait(1.0))
            stop.set()
            self.assertTrue(warmup_finished.wait(0.5))
            release_task.set()
            worker.join(1.0)

    def test_cleanup_cancels_streams_and_does_not_wait_forever_for_executor(self):
        calls = []
        controller = AIPluginController(
            request_service=SimpleNamespace(),
            media_ingress=SimpleNamespace(),
            stream_dispatcher=SimpleNamespace(),
            cai_client=SimpleNamespace(shutdown=lambda: calls.append("client")),
            event_loop_runner=SimpleNamespace(shutdown=lambda: calls.append("loop")),
            build_error_response=lambda *_: None,
        )
        executor = SimpleNamespace(
            shutdown=lambda **kwargs: calls.append(("executor", kwargs))
        )

        controller.cleanup(executor)

        self.assertEqual(
            calls,
            ["client", "loop", ("executor", {"wait": False, "cancel_futures": True})],
        )


if __name__ == "__main__":
    unittest.main()
