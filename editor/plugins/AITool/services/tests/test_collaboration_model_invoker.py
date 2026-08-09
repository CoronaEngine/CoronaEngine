from __future__ import annotations

import threading
import time
import unittest

from editor.plugins.AITool.services.collaboration_model_invoker import (
    CollaborationInvocationSaturated,
    CollaborationInvocationTimeout,
    CollaborationModelInvoker,
)


class CollaborationModelInvokerTests(unittest.TestCase):
    def test_completed_value_is_returned(self) -> None:
        invoker = CollaborationModelInvoker()

        result = invoker.invoke(
            room_id="room-ready",
            attempt_id="attempt-ready",
            stage_token="planning",
            deadline_s=0.5,
            call=lambda: "ready",
        )

        self.assertEqual(result, "ready")
        self.assertFalse(invoker.active("room-ready"))

    def test_timeout_discards_late_result_and_bounds_same_room(self) -> None:
        invoker = CollaborationModelInvoker()
        release = threading.Event()
        late = threading.Event()

        def call() -> str:
            release.wait(1.0)
            return "late"

        with self.assertRaises(CollaborationInvocationTimeout):
            invoker.invoke(
                room_id="room-timeout",
                attempt_id="attempt-timeout",
                stage_token="program",
                deadline_s=0.02,
                call=call,
                on_late_result=lambda _state: late.set(),
            )

        self.assertTrue(invoker.active("room-timeout"))
        with self.assertRaises(CollaborationInvocationSaturated):
            invoker.invoke(
                room_id="room-timeout",
                attempt_id="attempt-retry",
                stage_token="program",
                deadline_s=0.1,
                call=lambda: "must-not-run",
            )

        release.set()
        self.assertTrue(late.wait(0.5))
        for _ in range(20):
            if not invoker.active("room-timeout"):
                break
            time.sleep(0.01)
        self.assertFalse(invoker.active("room-timeout"))

    def test_underlying_error_is_preserved(self) -> None:
        invoker = CollaborationModelInvoker()

        def fail() -> None:
            raise ValueError("invalid model result")

        with self.assertRaisesRegex(ValueError, "invalid model result"):
            invoker.invoke(
                room_id="room-error",
                attempt_id="attempt-error",
                stage_token="art",
                deadline_s=0.5,
                call=fail,
            )


if __name__ == "__main__":
    unittest.main()
