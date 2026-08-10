import importlib.util
import ast
import sys
import threading
import unittest
from pathlib import Path
from types import ModuleType, SimpleNamespace
from unittest.mock import patch


class PythonScriptServiceRegistryTests(unittest.TestCase):
    def _load_registry(self):
        corona_editor_module = ModuleType("runtime.editor_host")
        corona_editor_module.CoronaEditor = SimpleNamespace(register_page=lambda *_: None)
        registry_path = Path(__file__).resolve().parents[1] / "registry.py"
        spec = importlib.util.spec_from_file_location(
            "test_python_script_service_registry",
            registry_path,
        )
        module = importlib.util.module_from_spec(spec)
        with patch.dict(
            sys.modules,
            {"runtime.editor_host": corona_editor_module},
        ):
            spec.loader.exec_module(module)
        return module

    def test_project_archive_service_is_registered_for_native_project_opening(self):
        registry = self._load_registry()

        self.assertIn("ProjectArchive", registry.PYTHON_SCRIPT_SERVICES)
        self.assertEqual(
            registry.PYTHON_SCRIPT_SERVICES["ProjectArchive"],
            ("plugins.ProjectArchive.main", "ProjectArchive"),
        )
        self.assertEqual(
            next(iter(registry.PYTHON_SCRIPT_SERVICES)),
            "ProjectArchive",
        )

    def test_active_service_list_is_explicit(self):
        registry = self._load_registry()

        self.assertEqual(
            registry.ACTIVE_PYTHON_SCRIPT_SERVICES,
            ("ProjectArchive", "AITool", "ScratchTool"),
        )
        self.assertFalse(hasattr(registry, "LEGACY_PYTHON_SCRIPT_SERVICES"))

    def test_project_launcher_registry_uses_canonical_plugin_owner(self):
        registry = self._load_registry()

        self.assertEqual(
            registry.PYTHON_SCRIPT_SERVICES["ProjectLauncher"],
            ("plugins.ProjectLauncher.main", "ProjectLauncher"),
        )

    def test_project_archive_registry_uses_plugin_owner(self):
        registry = self._load_registry()

        self.assertEqual(
            registry.PYTHON_SCRIPT_SERVICES["ProjectArchive"],
            ("plugins.ProjectArchive.main", "ProjectArchive"),
        )

    def test_later_services_register_when_an_earlier_import_fails(self):
        registry = self._load_registry()
        registry.PYTHON_SCRIPT_SERVICES = {
            "BrokenService": ("services.broken", "BrokenService"),
            "ProjectLauncher": ("services.project_launcher", "ProjectLauncher"),
        }
        project_launcher = object()
        registered_pages = []

        def import_service(module_path):
            if module_path == "services.broken":
                raise RuntimeError("broken dependency")
            return SimpleNamespace(ProjectLauncher=project_launcher)

        registry.CoronaEditor.register_page = (
            lambda service_name, service: registered_pages.append(
                (service_name, service),
            )
        )

        with patch.object(registry, "import_module", side_effect=import_service):
            with self.assertLogs(registry.logger, level="ERROR") as logs:
                registered = registry.register_python_script_services()

        self.assertEqual(registered, ["ProjectLauncher"])
        self.assertEqual(
            [(name, service._target) for name, service in registered_pages],
            [("ProjectLauncher", project_launcher)],
        )
        self.assertIn("BrokenService", "\n".join(logs.output))
        self.assertIn("services.broken", "\n".join(logs.output))

    def test_aitool_initializes_in_background_and_reports_initializing(self):
        registry = self._load_registry()
        registry.PYTHON_SCRIPT_SERVICES = {
            "ProjectArchive": ("services.archive", "ProjectArchive"),
            "AITool": ("services.ai", "AITool"),
        }
        registry.LAZY_PYTHON_SCRIPT_SERVICES = {"AITool"}
        imported = []
        initialized = []
        registered_pages = {}
        import_started = threading.Event()
        allow_import = threading.Event()
        archive_service = SimpleNamespace(parse=lambda payload: payload)
        ai_service = SimpleNamespace(submit_request=lambda payload: {"ok": payload})

        def import_service(module_path):
            imported.append(module_path)
            if module_path == "services.archive":
                return SimpleNamespace(ProjectArchive=archive_service)
            import_started.set()
            self.assertTrue(allow_import.wait(2.0))
            return SimpleNamespace(
                AITool=ai_service,
                initialize_script_service=lambda: initialized.append("AITool"),
            )

        registry.CoronaEditor.register_page = (
            lambda service_name, service: registered_pages.__setitem__(
                service_name,
                service,
            )
        )

        with patch.object(registry, "import_module", side_effect=import_service):
            registered = registry.register_python_script_services()
            self.assertTrue(import_started.wait(1.0))
            self.assertEqual(initialized, [])
            self.assertEqual(
                registered_pages["AITool"].submit_request("hello"),
                {
                    "success": False,
                    "status": "initializing",
                    "message": "AITool is initializing",
                },
            )
            allow_import.set()
            self.assertTrue(
                registered_pages["AITool"].wait_for_initialization(2.0),
            )
            self.assertEqual(initialized, ["AITool"])
            self.assertEqual(
                registered_pages["AITool"].submit_request("hello"),
                {"ok": "hello"},
            )

        self.assertEqual(registered, ["ProjectArchive", "AITool"])
        self.assertEqual(imported, ["services.archive", "services.ai"])

    def test_lazy_service_can_publish_target_before_background_initializer_finishes(self):
        registry = self._load_registry()
        initializer_started = threading.Event()
        allow_initializer = threading.Event()
        target = SimpleNamespace(submit_request=lambda payload: {"ok": payload})

        def initialize_script_service(stop_token):
            initializer_started.set()
            while not allow_initializer.is_set():
                if stop_token.is_set():
                    return False
                allow_initializer.wait(0.01)
            return True

        module = SimpleNamespace(
            AITool=target,
            INITIALIZE_AFTER_PUBLISH=True,
            initialize_script_service=initialize_script_service,
        )
        service = registry.LazyPythonScriptService("AITool", "services.ai", "AITool")
        with patch.object(registry, "import_module", return_value=module):
            service.start_background_load()
            self.assertTrue(initializer_started.wait(1.0))
            self.assertIs(service.target, target)
            self.assertEqual(service.state, "ready")
            self.assertEqual(service.submit_request("hello"), {"ok": "hello"})
            allow_initializer.set()
            self.assertTrue(service.wait_for_initialization(1.0))

        self.assertEqual(service.state, "ready")

    def test_repeated_service_registration_is_idempotent(self):
        registry = self._load_registry()
        registry.PYTHON_SCRIPT_SERVICES = {
            "AITool": ("services.ai", "AITool"),
        }
        registry.LAZY_PYTHON_SCRIPT_SERVICES = {"AITool"}
        registered_pages = []
        target = SimpleNamespace(submit_request=lambda payload: {"ok": payload})
        registry.CoronaEditor.register_page = (
            lambda service_name, service: registered_pages.append((service_name, service))
        )

        with patch.object(registry, "import_module", return_value=SimpleNamespace(AITool=target)):
            self.assertEqual(registry.register_python_script_services(), ["AITool"])
            self.assertEqual(registry.register_python_script_services(), [])

        self.assertEqual([name for name, _service in registered_pages], ["AITool"])

    def test_active_registration_does_not_start_legacy_scene_tools_service(self):
        registry = self._load_registry()
        registry.PYTHON_SCRIPT_SERVICES = {
            "SceneTools": ("plugins.SceneTools.main", "SceneTools"),
            "AITool": ("services.ai", "AITool"),
        }
        registry.ACTIVE_PYTHON_SCRIPT_SERVICES = ("AITool",)
        registered_pages = []

        registry.CoronaEditor.register_page = (
            lambda service_name, service: registered_pages.append(service_name)
        )

        with patch.object(
            registry,
            "import_module",
            side_effect=lambda module_path: SimpleNamespace(
                SceneTools=object() if module_path == "plugins.SceneTools.main" else None,
                AITool=object(),
            ),
        ) as import_service:
            registered = registry.register_active_python_script_services()

        self.assertEqual(registered, ["AITool"])
        self.assertEqual(registered_pages, ["AITool"])
        import_service.assert_called_once_with("services.ai")

    def test_aitool_uses_post_publish_initialization_without_import_time_worker_start(self):
        source_path = Path(__file__).resolve().parents[2] / "plugins" / "AITool" / "main.py"
        source = source_path.read_text(encoding="utf-8")
        tree = ast.parse(source)

        post_publish = [
            node
            for node in tree.body
            if isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id == "INITIALIZE_AFTER_PUBLISH" for target in node.targets)
        ]
        self.assertTrue(post_publish, "AITool must publish its lightweight target before initialization")
        self.assertTrue(
            any(isinstance(node.value, ast.Constant) and node.value.value is True for node in post_publish),
        )
        self.assertNotIn("AITool._lanchat_agent_worker.start()", source)

    def test_lazy_service_failure_is_degraded_and_does_not_retry(self):
        registry = self._load_registry()
        imported = []

        def import_service(module_path):
            imported.append(module_path)
            raise RuntimeError("missing AI dependency")

        service = registry.LazyPythonScriptService(
            "AITool",
            "services.ai",
            "AITool",
        )
        with self.assertLogs(registry.logger, level="ERROR") as logs:
            with patch.object(registry, "import_module", side_effect=import_service):
                service.start_background_load()
                self.assertTrue(service.wait_for_initialization(2.0))
                service.start_background_load()

        self.assertEqual(imported, ["services.ai"])
        self.assertIn("missing AI dependency", "\n".join(logs.output))
        self.assertEqual(
            service.submit_request("hello"),
            {
                "success": False,
                "status": "degraded",
                "message": "AITool initialization failed: missing AI dependency",
            },
        )

    def test_lazy_service_shutdown_prevents_late_target_publication(self):
        registry = self._load_registry()
        import_started = threading.Event()
        allow_import = threading.Event()
        cleanup_calls = []
        target = SimpleNamespace(cleanup=lambda: cleanup_calls.append("cleanup"))

        def import_service(_module_path):
            import_started.set()
            self.assertTrue(allow_import.wait(2.0))
            return SimpleNamespace(AITool=target)

        service = registry.LazyPythonScriptService("AITool", "services.ai", "AITool")
        with patch.object(registry, "import_module", side_effect=import_service):
            service.start_background_load()
            self.assertTrue(import_started.wait(1.0))
            service.request_shutdown()
            allow_import.set()
            snapshot = service.shutdown(2.0)

        self.assertEqual(service.state, "stopped")
        self.assertIsNone(service.target)
        # Cancellation happened before import_service returned the module, so
        # the service never took ownership of its target and must not clean it.
        self.assertEqual(cleanup_calls, [])
        self.assertFalse(snapshot["thread_alive"])

    def test_lazy_service_passes_stop_token_to_cooperative_initializer(self):
        registry = self._load_registry()
        initializer_started = threading.Event()
        cleanup_calls = []
        target = SimpleNamespace(cleanup=lambda: cleanup_calls.append("cleanup"))

        def initialize_script_service(stop_token):
            initializer_started.set()
            stop_token.wait(2.0)

        module = SimpleNamespace(
            AITool=target,
            initialize_script_service=initialize_script_service,
        )
        service = registry.LazyPythonScriptService("AITool", "services.ai", "AITool")
        with patch.object(registry, "import_module", return_value=module):
            service.start_background_load()
            self.assertTrue(initializer_started.wait(1.0))
            snapshot = service.shutdown(0.5)

        self.assertEqual(snapshot["state"], "stopped")
        self.assertFalse(snapshot["thread_alive"])
        self.assertEqual(cleanup_calls, ["cleanup"])

    def test_lazy_service_shutdown_cancels_python_module_import(self):
        registry = self._load_registry()
        import_started = threading.Event()

        def import_service(_module_path):
            import_started.set()
            value = 0
            while True:
                value += 1

        service = registry.LazyPythonScriptService("AITool", "services.ai", "AITool")
        with patch.object(registry, "import_module", side_effect=import_service):
            service.start_background_load()
            self.assertTrue(import_started.wait(1.0))
            snapshot = service.shutdown(1.0)

        self.assertEqual(snapshot["state"], "stopped")
        self.assertFalse(snapshot["thread_alive"])
        self.assertEqual(snapshot["error"], "")

    def test_registered_services_shutdown_in_reverse_order(self):
        registry = self._load_registry()
        shutdown_order = []

        class Service:
            def __init__(self, name):
                self.name = name

            def request_shutdown(self):
                shutdown_order.append(f"request:{self.name}")

            def shutdown(self, _deadline):
                shutdown_order.append(f"shutdown:{self.name}")
                return {"service": self.name, "state": "stopped"}

        registry._managed_services[:] = [Service("first"), Service("second")]
        snapshots = registry.shutdown_python_script_services(1.0)

        self.assertEqual(
            shutdown_order,
            ["request:second", "request:first", "shutdown:second", "shutdown:first"],
        )
        self.assertEqual([item["service"] for item in snapshots], ["second", "first"])

    def test_eager_service_forwards_shutdown_deadline_and_snapshot(self):
        registry = self._load_registry()
        deadlines = []

        class Target:
            @classmethod
            def shutdown(cls, deadline):
                deadlines.append(deadline)
                return {
                    "service": "ScratchTool",
                    "state": "stop_timeout",
                    "thread_alive": True,
                }

        service = registry.PythonScriptService("ScratchTool", Target)
        snapshot = service.shutdown(0.25)

        self.assertEqual(deadlines, [0.25])
        self.assertEqual(snapshot["state"], "stop_timeout")
        self.assertTrue(snapshot["thread_alive"])


if __name__ == "__main__":
    unittest.main()
