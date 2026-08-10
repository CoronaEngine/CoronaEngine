import importlib.util
import sys
import unittest
from pathlib import Path
from types import ModuleType, SimpleNamespace
from unittest.mock import patch


class CoronaEditorLifecycleTests(unittest.TestCase):
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

    def test_shutdown_runtime_stops_services_and_clears_dispatch_state(self):
        editor = self._load_editor()
        calls = []
        scripts = SimpleNamespace(shutdown=lambda: calls.append("scripts"))
        registry = ModuleType("runtime.registry")
        registry.shutdown_python_script_services = (
            lambda timeout: calls.append(("services", timeout)) or [{"state": "stopped"}]
        )
        editor.scripts_mgr = scripts
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
        self.assertEqual(calls, [("services", 2.0), "scripts", "dispatcher"])
        self.assertEqual(editor.module_list, {})
        self.assertEqual(editor._runtime_state, "stopped")

    def test_cpp_python_owner_does_not_use_global_initialized_state_for_concurrency(self):
        repo_root = Path(__file__).resolve().parents[3]
        source = (
            repo_root / "src" / "systems" / "script" / "python" / "python_api.cpp"
        ).read_text(encoding="utf-8")
        self.assertNotIn("Py_IsInitialized()", source)

    def test_cpp_shutdown_receipt_outlives_python_service_deadline(self):
        repo_root = Path(__file__).resolve().parents[3]
        source = (
            repo_root / "src" / "systems" / "script" / "python" / "python_api.cpp"
        ).read_text(encoding="utf-8")
        shutdown_start = source.index("void PythonAPI::begin_shutdown()")
        shutdown_end = source.index("PythonLifecycleSnapshot", shutdown_start)
        shutdown_body = source[shutdown_start:shutdown_end]
        self.assertIn("milliseconds(2500)", shutdown_body)
        self.assertNotIn("milliseconds(1500)", shutdown_body)

    def test_runtime_update_has_native_phase_markers_and_stack_watchdog(self):
        repo_root = Path(__file__).resolve().parents[3]
        source = (
            repo_root / "editor" / "runtime" / "editor_host.py"
        ).read_text(encoding="utf-8")
        update_start = source.index("    def _set_native_runtime_phase(cls, phase):")
        update_end = source.index("\n    def show_log_on_js", update_start)
        update_body = source[update_start:update_end]
        for phase in (
            '"script_initialize"',
            '"script_update"',
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
            repo_root / "src" / "systems" / "script" / "python" / "python_api.cpp"
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
