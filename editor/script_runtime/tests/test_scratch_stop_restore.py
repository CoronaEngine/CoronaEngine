import threading
import unittest
from unittest import mock

from script_runtime.blockly import main as blockly_main
from script_runtime.engine import corona_engine as corona_engine_scratch


class ScratchStopRestoreTests(unittest.TestCase):
    def setUp(self):
        self.tool = blockly_main.ScratchTool
        state = dict(self.tool._exec_state)
        state.update(
            status="idle", outcome="", error="", inputLocked=False,
            snapshotCaptured=False, restoreStatus="idle", restoreError="",
        )
        self.enterContext(mock.patch.multiple(
            self.tool,
            _exec_thread=None,
            _exec_context_id=None,
            _exec_state_snapshot=None,
            _exec_input_locked=False,
            _exec_lock=threading.RLock(),
            _exec_state=state,
        ))
        self.camera = self.enterContext(mock.patch.object(
            blockly_main, "set_editor_camera_input_enabled",
        ))
        self.cancel_saves = self.enterContext(mock.patch.object(
            blockly_main, "cancel_pending_auto_saves",
        ))
        self.restore = self.enterContext(mock.patch.object(
            corona_engine_scratch, "restore_runtime_scene_state",
        ))
        self.request_stop = self.enterContext(mock.patch.object(
            corona_engine_scratch, "request_stop",
        ))
        self.children = self.enterContext(mock.patch.object(
            corona_engine_scratch, "active_child_threads", return_value=[],
        ))
        self.isolate = self.enterContext(mock.patch.object(
            corona_engine_scratch, "isolate_context",
        ))
        self.enterContext(mock.patch.object(
            corona_engine_scratch, "runtime_context_snapshot", return_value={},
        ))

    def assert_no_restore_needed(self, result):
        self.assertEqual(result, {
            "status": "stopped", "restored": False,
            "snapshotCaptured": False, "restoreStatus": "idle",
        })
        self.assertEqual(self.tool._exec_state["restoreError"], "")
        self.assertEqual(self.tool._exec_state["error"], "")
        self.assertFalse(self.tool._exec_state["snapshotCaptured"])
        self.assertFalse(self.tool._exec_state["inputLocked"])
        self.assertIsNone(self.tool._exec_state_snapshot)
        self.assertIsNone(self.tool._exec_thread)
        self.assertIsNone(self.tool._exec_context_id)

    def test_stop_before_any_execution_needs_no_restore(self):
        self.assert_no_restore_needed(self.tool.stop_script_execution(True))
        self.restore.assert_not_called()
        self.cancel_saves.assert_not_called()
        self.camera.assert_called_once_with(True, reason="node_graph")

    def test_no_snapshot_clears_stale_restore_error(self):
        self.tool._exec_state.update(
            snapshotCaptured=True, restoreStatus="error",
            restoreError="no runtime snapshot", error="previous restore error",
        )
        self.assert_no_restore_needed(self.tool.stop_script_execution(True))
        self.restore.assert_not_called()
        self.cancel_saves.assert_not_called()

    def test_restore_then_stop_again_is_idempotent(self):
        snapshot = {"binding_mode": "native_editor", "scene_name": "test.scene"}
        self.tool._exec_state_snapshot = snapshot
        self.tool._exec_state["snapshotCaptured"] = True
        first = self.tool.stop_script_execution(True)
        self.assertEqual(first, {
            "status": "stopped", "restored": True,
            "snapshotCaptured": False, "restoreStatus": "restored",
        })
        self.restore.assert_called_once_with(snapshot)
        self.cancel_saves.assert_called_once_with()
        self.assert_no_restore_needed(self.tool.stop_script_execution(True))
        self.restore.assert_called_once_with(snapshot)
        self.cancel_saves.assert_called_once_with()

    def test_restore_failure_keeps_snapshot_and_diagnostics_for_retry(self):
        snapshot = {"binding_mode": "native_editor", "scene_name": "test.scene"}
        self.tool._exec_state_snapshot = snapshot
        self.tool._exec_state["snapshotCaptured"] = True
        self.restore.side_effect = RuntimeError("scene restore failed")
        result = self.tool.stop_script_execution(True)
        self.assertEqual(result["status"], "stopped")
        self.assertFalse(result["restored"])
        self.assertTrue(result["snapshotCaptured"])
        self.assertEqual(result["restoreStatus"], "error")
        self.assertEqual(result["restoreError"], "scene restore failed")
        self.assertEqual(self.tool._exec_state["restoreError"], "scene restore failed")
        self.assertIs(self.tool._exec_state_snapshot, snapshot)
        self.restore.side_effect = None
        self.assertTrue(self.tool.stop_script_execution(True)["restored"])
        self.assertIsNone(self.tool._exec_state_snapshot)

    def test_falsey_non_none_snapshot_is_not_treated_as_missing(self):
        self.restore.side_effect = RuntimeError("invalid snapshot")
        for snapshot in ({}, [], "", False):
            with self.subTest(snapshot=snapshot):
                self.restore.reset_mock()
                self.tool._exec_state_snapshot = snapshot
                result = self.tool.stop_script_execution(True)
                self.restore.assert_called_once_with(snapshot)
                self.assertEqual(result["restoreError"], "invalid snapshot")
                self.assertTrue(result["snapshotCaptured"])
                self.assertIs(self.tool._exec_state_snapshot, snapshot)

    def test_unfinished_main_or_child_thread_blocks_restore(self):
        snapshot = {"binding_mode": "native_editor"}
        for role in ("main", "child"):
            with self.subTest(role=role):
                thread = mock.Mock()
                thread.name = f"pending-{role}"
                thread.is_alive.return_value = True
                self.tool._exec_state_snapshot = snapshot
                self.tool._exec_context_id = "test-context"
                self.tool._exec_thread = thread if role == "main" else None
                self.children.return_value = [thread] if role == "child" else []
                result = self.tool.stop_script_execution(True)
                self.assertEqual(result["status"], "error")
                self.assertEqual(result["pendingThreads"], [thread.name])
                self.assertFalse(result["restored"])
                self.assertEqual(self.tool._exec_state["outcome"], "stop_timeout")
                self.assertIs(self.tool._exec_state_snapshot, snapshot)
                self.restore.assert_not_called()
                self.cancel_saves.assert_not_called()
                self.isolate.assert_called()

    def test_missing_snapshot_does_not_bypass_live_threads(self):
        for role in ("main", "child"):
            with self.subTest(role=role):
                thread = mock.Mock()
                thread.name = f"pending-{role}"
                thread.is_alive.return_value = True
                self.tool._exec_context_id = "test-context"
                self.tool._exec_thread = thread if role == "main" else None
                self.children.return_value = [thread] if role == "child" else []
                result = self.tool.stop_script_execution(True)
                self.assertEqual(result["status"], "error")
                self.assertEqual(result["pendingThreads"], [thread.name])
                self.assertFalse(result["restored"])
                self.restore.assert_not_called()

                # Once the worker really exits, retry succeeds even without a snapshot.
                thread.is_alive.return_value = False
                self.children.return_value = []
                self.assert_no_restore_needed(self.tool.stop_script_execution(True))

    def test_cooperative_thread_finishes_before_restore(self):
        thread = mock.Mock()
        thread.name = "cooperative-main"
        thread.is_alive.side_effect = [True, False]
        self.tool._exec_thread = thread
        self.tool._exec_context_id = "test-context"
        snapshot = {"binding_mode": "native_editor"}
        self.tool._exec_state_snapshot = snapshot

        def restore_after_stop(value):
            self.request_stop.assert_called_once_with("test-context")
            thread.join.assert_called_once_with(timeout=0.5)
            self.children.assert_called_once_with({"test-context"})
            self.assertIs(value, snapshot)

        self.restore.side_effect = restore_after_stop
        self.assertTrue(self.tool.stop_script_execution(True)["restored"])
        self.isolate.assert_not_called()

    def test_stop_without_restore_keeps_existing_discard_behavior(self):
        self.tool._exec_state_snapshot = {"binding_mode": "native_editor"}
        self.assert_no_restore_needed(self.tool.stop_script_execution(False))
        self.restore.assert_not_called()
        self.cancel_saves.assert_not_called()


if __name__ == "__main__":
    unittest.main()
