import importlib.util
import sys
import unittest
from pathlib import Path
from types import ModuleType, SimpleNamespace
from unittest.mock import Mock, patch


EDITOR_ROOT = Path(__file__).resolve().parents[2]
ENGINE_ROOT = EDITOR_ROOT.parent


class CoronaEditorLifecycleTests(unittest.TestCase):
    def test_structured_service_call_does_not_serialize_values(self):
        editor = self._load_editor()
        value = {"id": 2**64 - 1, "text": "嵌套\x00值", "nodes": [True, None, {"x": 1.5}]}
        editor.module_list = {"Fixture": SimpleNamespace(echo=lambda payload: payload)}
        with patch("json.loads", side_effect=AssertionError("JSON transport forbidden")), \
                patch("json.dumps", side_effect=AssertionError("JSON transport forbidden")):
            reply = editor.dispatch_script_values_from_cpp("Fixture", "echo", [value])
        self.assertTrue(reply["success"])
        self.assertIs(reply["data"], value)

    def test_structured_service_errors_remain_explicit(self):
        editor = self._load_editor()
        editor.module_list = {"Fixture": SimpleNamespace(fail=Mock(side_effect=ValueError("fixture failed")))}
        for module, function, args in [("Missing", "fail", []), ("Fixture", "missing", []),
                                       ("Fixture", "fail", []), ("Fixture", "fail", "not an argument array")]:
            reply = editor.dispatch_script_values_from_cpp(module, function, args)
            self.assertFalse(reply["success"])
            self.assertIsInstance(reply["error"], str)

    def test_registered_text_adapter_retains_its_contract_and_rejects_editor_api_payloads(self):
        import json
        editor = self._load_editor()
        echo = Mock(return_value={"status": "ready"})
        editor.module_list = {"Fixture": SimpleNamespace(echo=echo)}
        # The fixture's response helpers return dictionaries so the adapter's
        # choice of success/error helper can be checked independently.
        reply = editor.dispatch_script_request_from_cpp(json.dumps({"module": "Fixture", "function": "echo", "args": [42]}))
        self.assertEqual(reply, {"data": {"status": "ready"}})
        echo.assert_called_once_with(42)
        rejected = editor.dispatch_script_request_from_cpp(json.dumps({"api": "Fixture.echo"}))
        self.assertIn("not accepted", rejected["error"])
        self.assertEqual(echo.call_count, 1)

    def _load_editor(self):
        engine = SimpleNamespace()
        corona_engine = ModuleType("runtime.native_engine")
        corona_engine.get_corona_engine = lambda: engine
        settings = ModuleType("config.project_state")
        settings.settings_manager = SimpleNamespace(set_active_project=lambda *_: True)
        responses = ModuleType("runtime.response_utils")
        responses.create_error_response = lambda message: {"error": message}
        responses.create_success_response = lambda value: {"data": value}
        editor_path = Path(__file__).resolve().parents[1] / "editor_host.py"
        spec = importlib.util.spec_from_file_location("test_corona_editor_lifecycle_module", editor_path)
        module = importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules, {
            "runtime.native_engine": corona_engine,
            "config.project_state": settings,
            "runtime.response_utils": responses,
        }):
            spec.loader.exec_module(module)
        return module.CoronaEditor

    def test_runtime_update_is_independent_of_the_retired_log_tool(self):
        editor = self._load_editor()
        self.assertFalse(hasattr(editor, "show_log_on_js"))
        self.assertFalse(hasattr(editor, "emit_editor_event"))
        self.assertTrue(editor.initialize_runtime())
        self.assertTrue(editor.start_runtime())
        engine = SimpleNamespace(drain_input_events=Mock(return_value=[]))
        with patch.object(editor, "_arm_runtime_watchdog"), patch.dict(sys.modules, {"CoronaEngine": engine}):
            self.assertTrue(editor.update_runtime())
        engine.drain_input_events.assert_called_once_with()

    def test_typed_queue_preserves_keyboard_display_key_and_full_mouse_context(self):
        editor = self._load_editor()
        editor.initialize_runtime()
        editor.start_runtime()
        key = SimpleNamespace(type=0, arg0='KeyJ', arg1='Shift', arg2='d')
        up = SimpleNamespace(type=1, arg0='KeyJ', arg1='d')
        mouse = SimpleNamespace(type=2, arg0='click', arg1='LeftButton', arg3=50, arg4=60,
                                viewport_x=5, viewport_y=6, viewport_width=400, viewport_height=200, picked_actor='Picked')
        engine = SimpleNamespace(drain_input_events=Mock(return_value=[key, up, mouse]))
        file_runner = ModuleType('script_runtime.file_runner')
        scripts = file_runner.FileScripts = SimpleNamespace(key_event=Mock(), key_release=Mock(), mouse_event=Mock())
        with patch.object(editor, '_arm_runtime_watchdog'), patch.dict(sys.modules, {'CoronaEngine': engine, 'script_runtime.file_runner': file_runner}):
            self.assertTrue(editor.update_runtime())
        scripts.key_event.assert_called_once_with('KeyJ', 'Shift', 'd')
        scripts.key_release.assert_called_once_with('KeyJ', 'd')
        scripts.mouse_event.assert_called_once_with('click', 'LeftButton', 50, 60, 5, 6, 400, 200, 'Picked')

    def test_shutdown_runtime_stops_services_and_clears_dispatch_state(self):
        editor = self._load_editor()
        calls = []
        registry = ModuleType("runtime.registry")
        registry.shutdown_python_script_services = (
            lambda timeout: calls.append(("services", timeout)) or [{"state": "stopped"}]
        )
        editor.module_list = {"AITool": object(), "SceneTools": object()}
        editor._runtime_state = "running"
        editor._runtime_initialized = True
        editor._runtime_started = True
        editor.unregister_script_dispatcher = classmethod(lambda cls: calls.append("dispatcher"))
        runtime = ModuleType("runtime")
        runtime.registry = registry

        with patch.dict(sys.modules, {"runtime": runtime, "runtime.registry": registry}):
            result = editor.shutdown_runtime()

        self.assertTrue(result)
        self.assertEqual(result["runtime_state"], "stopped")
        self.assertEqual(result["services"], [{"state": "stopped"}])
        self.assertIn("python_threads", result)
        self.assertTrue(all("name" in item and "ident" in item for item in result["python_threads"]))
        self.assertEqual(calls, [("services", 2.0), "dispatcher"])
        self.assertEqual(editor.module_list, {})
        self.assertEqual(editor._runtime_state, "stopped")

    def test_cpp_python_owner_does_not_use_global_initialized_state_for_concurrency(self):
        source = (
            ENGINE_ROOT / "src" / "systems" / "script" / "python" / "python_api.cpp"
        ).read_text(encoding="utf-8")
        self.assertNotIn("Py_IsInitialized()", source)

    def test_cpp_shutdown_receipt_outlives_python_service_deadline(self):
        source = (
            ENGINE_ROOT / "src" / "systems" / "script" / "python" / "python_api.cpp"
        ).read_text(encoding="utf-8")
        shutdown_start = source.index("void PythonAPI::begin_shutdown()")
        shutdown_end = source.index("PythonLifecycleSnapshot", shutdown_start)
        shutdown_body = source[shutdown_start:shutdown_end]
        self.assertIn("milliseconds(2500)", shutdown_body)
        self.assertNotIn("milliseconds(1500)", shutdown_body)

    def test_runtime_update_has_native_phase_markers_and_stack_watchdog(self):
        source = (
            EDITOR_ROOT / "runtime" / "editor_host.py"
        ).read_text(encoding="utf-8")
        update_start = source.index("    def _set_native_runtime_phase(cls, phase):")
        update_end = source.index("\ndef set_editor_camera_input_enabled(", update_start)
        update_body = source[update_start:update_end]
        for phase in (
            '"input_dispatch"',
            '"idle"',
        ):
            self.assertIn(f"_set_native_runtime_phase({phase})", update_body)
        self.assertIn("def _arm_runtime_watchdog", update_body)
        self.assertIn("faulthandler.enable(file=cls._runtime_watchdog_file, all_threads=True)", update_body)
        self.assertIn("faulthandler.dump_traceback_later", update_body)
        self.assertIn("file=cls._runtime_watchdog_file", update_body)
        self.assertIn("def _cancel_runtime_watchdog", update_body)
        self.assertIn("faulthandler.cancel_dump_traceback_later", update_body)
        update_runtime_start = update_body.index("    def update_runtime(cls):")
        update_runtime_body = update_body[update_runtime_start:]
        self.assertIn("cls._arm_runtime_watchdog()", update_runtime_body)

        shutdown_start = source.index("    def shutdown_runtime(cls):")
        shutdown_end = source.index("\n    @classmethod", shutdown_start + 1)
        shutdown_body = source[shutdown_start:shutdown_end]
        self.assertIn("cls._cancel_runtime_watchdog()", shutdown_body)

        cpp_source = (
            ENGINE_ROOT / "src" / "systems" / "script" / "python" / "python_api.cpp"
        ).read_text(encoding="utf-8")
        invoke_start = cpp_source.index("void PythonAPI::invokeEntry")
        invoke_end = cpp_source.index("void PythonAPI::sendMessage", invoke_start)
        invoke_body = cpp_source[invoke_start:invoke_end]
        self.assertLess(
            invoke_body.index('set_execution_phase("gil_wait:editor_update")'),
            invoke_body.index("nanobind::gil_scoped_acquire gil"),
        )
        self.assertGreater(
            invoke_body.index('set_execution_phase("editor_update")'),
            invoke_body.index("nanobind::gil_scoped_acquire gil"),
        )


if __name__ == "__main__":
    unittest.main()
