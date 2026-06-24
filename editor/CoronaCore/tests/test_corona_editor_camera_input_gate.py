import sys
import types
import unittest

from CoronaCore.core.corona_editor import CoronaEditor


class CoronaEditorCameraInputGateTests(unittest.TestCase):
    def test_editor_camera_input_gate_syncs_camera_follow_controller(self):
        calls = []
        fake_engine = types.SimpleNamespace(
            camera_follow_set_input_enabled=lambda enabled: calls.append(bool(enabled))
        )
        previous = sys.modules.get("CoronaEngine")
        sys.modules["CoronaEngine"] = fake_engine
        original_enabled = CoronaEditor._editor_camera_input_enabled
        original_locks = set(CoronaEditor._editor_camera_input_locks)
        try:
            CoronaEditor._editor_camera_input_enabled = True
            CoronaEditor._editor_camera_input_locks.clear()
            CoronaEditor.set_editor_camera_input_enabled(False, "focus")
            CoronaEditor.set_editor_camera_input_enabled(False, "preview")
            CoronaEditor.set_editor_camera_input_enabled(True, "focus")
            CoronaEditor.set_editor_camera_input_enabled(True, "preview")
        finally:
            CoronaEditor._editor_camera_input_enabled = original_enabled
            CoronaEditor._editor_camera_input_locks = original_locks
            if previous is None:
                sys.modules.pop("CoronaEngine", None)
            else:
                sys.modules["CoronaEngine"] = previous

        self.assertEqual(calls, [False, True])


if __name__ == "__main__":
    unittest.main()
