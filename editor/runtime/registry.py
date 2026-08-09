import logging
import inspect
import sys
import threading
import time
from importlib import import_module

from runtime.editor_host import CoronaEditor


# Blockly's Python runtime host and generated output owner are canonical here;
# the historical ``backend/script`` path remains a read-only compatibility
# fallback for old hosts and previously generated sessions.
_BLOCKLY_PACKAGE = "script_runtime"
logger = logging.getLogger(__name__)


PYTHON_SCRIPT_SERVICES = {
    "ProjectArchive": ("plugins.ProjectArchive.main", "ProjectArchive"),
    "AITool": ("plugins.AITool.main", "AITool"),
    "ScratchTool": (f"{_BLOCKLY_PACKAGE}.blockly.main", "ScratchTool"),
    "MainView": ("plugins.MainView.main", "MainView"),
    "ProjectLauncher": ("plugins.ProjectLauncher.main", "ProjectLauncher"),
    "FileManager": ("plugins.FileManager.main", "FileManager"),
    "ProjectSettings": ("plugins.ProjectSettings.main", "ProjectSettings"),
    "SceneDatas": ("plugins.SceneDatas.main", "SceneDatas"),
    "SceneTools": ("plugins.SceneTools.main", "SceneTools"),
}

CORE_PYTHON_SCRIPT_SERVICES = set()
LEGACY_PYTHON_SCRIPT_SERVICES = {
    "SceneDatas",
    "ProjectLauncher",
    "FileManager",
    "ProjectSettings",
    "MainView",
    "SceneTools",
}
LAZY_PYTHON_SCRIPT_SERVICES = {"AITool", "ProjectArchive"}
_managed_services = []
_registered_service_names = set()
_registry_shutdown_requested = threading.Event()
_initializer_monitor_lock = threading.Lock()
_initializer_monitor_targets = {}
_initializer_monitor_tool_id = None


class _ServiceInitializationCancelled(BaseException):
    """Internal cooperative cancellation that ordinary import handlers do not swallow."""


def _initializer_cancel_monitor(code, line):
    del code, line
    thread_id = threading.get_ident()
    stop_token = _initializer_monitor_targets.get(thread_id)
    if stop_token is not None and stop_token.is_set():
        # Monitoring callbacks can run while registry management code owns its
        # lock. The mapping itself is protected by the GIL, so do not re-enter
        # the non-recursive management mutex here.
        _initializer_monitor_targets.pop(thread_id, None)
        raise _ServiceInitializationCancelled()


def _install_initializer_cancel_monitor(thread_id, stop_token):
    global _initializer_monitor_tool_id
    if thread_id is None or not hasattr(sys, "monitoring"):
        return
    install = False
    tool_id = None
    with _initializer_monitor_lock:
        _initializer_monitor_targets[thread_id] = stop_token
        if _initializer_monitor_tool_id is None:
            for candidate in range(5, -1, -1):
                if sys.monitoring.get_tool(candidate) is None:
                    tool_id = candidate
                    _initializer_monitor_tool_id = candidate
                    install = True
                    break
    if install:
        try:
            sys.monitoring.use_tool_id(tool_id, "corona-service-shutdown")
            sys.monitoring.register_callback(
                tool_id, sys.monitoring.events.LINE, _initializer_cancel_monitor)
            sys.monitoring.set_events(tool_id, sys.monitoring.events.LINE)
        except Exception:
            with _initializer_monitor_lock:
                _initializer_monitor_targets.pop(thread_id, None)
                if _initializer_monitor_tool_id == tool_id:
                    _initializer_monitor_tool_id = None
            try:
                sys.monitoring.free_tool_id(tool_id)
            except Exception:
                pass
            logger.exception("Unable to install Python service cancellation monitor")


def _remove_initializer_cancel_monitor(thread_id):
    global _initializer_monitor_tool_id
    tool_id = None
    with _initializer_monitor_lock:
        _initializer_monitor_targets.pop(thread_id, None)
        if _initializer_monitor_tool_id is not None and not _initializer_monitor_targets:
            tool_id = _initializer_monitor_tool_id
            _initializer_monitor_tool_id = None
    if tool_id is not None:
        sys.monitoring.set_events(tool_id, 0)
        sys.monitoring.register_callback(tool_id, sys.monitoring.events.LINE, None)
        sys.monitoring.free_tool_id(tool_id)


class PythonScriptService:
    def __init__(self, module_name, target):
        self.module_name = module_name
        self._target = target

    def __getattr__(self, name):
        return getattr(self._target, name)

    def request_shutdown(self):
        callback = getattr(self._target, "request_shutdown", None)
        if callable(callback):
            callback()

    def shutdown(self, _timeout=0.0):
        callback = getattr(self._target, "cleanup", None)
        if callable(callback):
            callback()
            return {"service": self.module_name, "state": "stopped", "thread_alive": False}
        callback = getattr(self._target, "shutdown", None)
        if callable(callback):
            snapshot = callback(max(0.0, float(_timeout)))
            if isinstance(snapshot, dict):
                return snapshot
        return {"service": self.module_name, "state": "stopped", "thread_alive": False}

    def snapshot(self):
        return {"service": self.module_name, "state": "ready", "thread_alive": False}


class LazyPythonScriptService:
    def __init__(self, module_name, module_path, class_name):
        self.module_name = module_name
        self._module_path = module_path
        self._class_name = class_name
        self._target = None
        self._state = "cold"
        self._initialization_error = None
        self._state_lock = threading.Lock()
        self._initialization_finished = threading.Event()
        self._stop_requested = threading.Event()
        self._worker = None
        self._worker_ident = None
        self._cleaned = False

    @property
    def state(self):
        with self._state_lock:
            return self._state

    @property
    def target(self):
        with self._state_lock:
            return self._target

    def start_background_load(self):
        with self._state_lock:
            if self._state != "cold" or self._stop_requested.is_set():
                return
            self._state = "initializing"
            worker = threading.Thread(
                target=self._load_target,
                name=f"{self.module_name}Initializer",
                daemon=True,
            )
            self._worker = worker
        worker.start()

    def wait_for_initialization(self, timeout=None):
        self.start_background_load()
        return self._initialization_finished.wait(timeout)

    def _load_target(self):
        worker_ident = threading.get_ident()
        module = None
        target = None
        with self._state_lock:
            self._worker_ident = worker_ident
        try:
            if self._stop_requested.is_set():
                raise _ServiceInitializationCancelled()
            import_started = time.perf_counter()
            module = import_module(self._module_path)
            logger.info(
                "Python script service %s module import finished in %.1fms thread=%s",
                self.module_name,
                (time.perf_counter() - import_started) * 1000.0,
                worker_ident,
            )
            target = getattr(module, self._class_name)
            initialize_after_publish = bool(
                getattr(module, "INITIALIZE_AFTER_PUBLISH", False)
            )
            if initialize_after_publish:
                if self._stop_requested.is_set():
                    raise _ServiceInitializationCancelled()
                with self._state_lock:
                    self._target = target
                    self._state = "ready"
                logger.info(
                    "Python script service %s published before background initialization thread=%s",
                    self.module_name,
                    worker_ident,
                )
            initializer = getattr(module, "initialize_script_service", None)
            if callable(initializer):
                initialize_started = time.perf_counter()
                parameters = inspect.signature(initializer).parameters
                if "stop_token" in parameters:
                    initialized = initializer(stop_token=self._stop_requested)
                else:
                    initialized = initializer()
                logger.info(
                    "Python script service %s initialization finished in %.1fms thread=%s",
                    self.module_name,
                    (time.perf_counter() - initialize_started) * 1000.0,
                    worker_ident,
                )
                if initialized is False and not self._stop_requested.is_set():
                    raise RuntimeError(
                        f"Python script service {self.module_name} initializer returned false"
                    )
            if self._stop_requested.is_set():
                self._cleanup_target(target)
                with self._state_lock:
                    self._state = "stopped"
                return
            if not initialize_after_publish:
                with self._state_lock:
                    self._target = target
                    self._state = "ready"
            logger.info(
                "Python script service %s initialized in background",
                self.module_name,
            )
        except _ServiceInitializationCancelled:
            target = getattr(module, self._class_name, None) if module is not None else None
            if target is not None:
                self._cleanup_target(target)
            with self._state_lock:
                self._state = "stopped"
        except Exception as error:
            with self._state_lock:
                self._initialization_error = error
                self._state = "degraded"
            logger.exception(
                "Failed to initialize Python script service %s from %s.%s",
                self.module_name,
                self._module_path,
                self._class_name,
            )
        finally:
            _remove_initializer_cancel_monitor(worker_ident)
            with self._state_lock:
                self._worker_ident = None
            self._initialization_finished.set()

    def _cleanup_target(self, target):
        with self._state_lock:
            if self._cleaned:
                return
            self._cleaned = True
        callback = getattr(target, "cleanup", None)
        if not callable(callback):
            callback = getattr(target, "shutdown", None)
        if callable(callback):
            callback()

    def request_shutdown(self):
        self._stop_requested.set()
        with self._state_lock:
            target = self._target
            worker_ident = self._worker_ident
            if self._state == "cold":
                self._state = "stopped"
                self._initialization_finished.set()
            elif self._state not in ("stopped", "degraded"):
                self._state = "stop_requested"
        callback = getattr(target, "request_shutdown", None) if target is not None else None
        if callable(callback):
            callback()
        if worker_ident is not None:
            _install_initializer_cancel_monitor(worker_ident, self._stop_requested)

    def shutdown(self, timeout=2.0):
        self.request_shutdown()
        worker = self._worker
        if worker and worker is not threading.current_thread() and worker.is_alive():
            worker.join(timeout=max(0.0, float(timeout)))
        with self._state_lock:
            target = self._target
            worker_alive = bool(worker and worker.is_alive())
        if target is not None:
            self._cleanup_target(target)
        with self._state_lock:
            self._target = None
            self._state = "stop_timeout" if worker_alive else "stopped"
        return self.snapshot()

    def snapshot(self):
        worker = self._worker
        with self._state_lock:
            return {
                "service": self.module_name,
                "state": self._state,
                "thread_alive": bool(worker and worker.is_alive()),
                "error": str(self._initialization_error or ""),
            }

    def _unavailable_result(self, state, error):
        if state == "degraded":
            return {
                "success": False,
                "status": "degraded",
                "message": (
                    f"{self.module_name} initialization failed: {error}"
                ),
            }
        return {
            "success": False,
            "status": "initializing",
            "message": f"{self.module_name} is initializing",
        }

    def __getattr__(self, name):
        def invoke(*args, **kwargs):
            self.start_background_load()
            with self._state_lock:
                target = self._target
                state = self._state
                error = self._initialization_error
            if target is not None:
                return getattr(target, name)(*args, **kwargs)
            return self._unavailable_result(state, error)

        return invoke


def _register_python_script_services(service_names):
    registered = []
    for service_name in service_names:
        if _registry_shutdown_requested.is_set():
            break
        if service_name not in PYTHON_SCRIPT_SERVICES:
            continue
        if service_name in _registered_service_names:
            continue
        module_path, class_name = PYTHON_SCRIPT_SERVICES[service_name]
        try:
            if service_name in LAZY_PYTHON_SCRIPT_SERVICES:
                service = LazyPythonScriptService(
                    service_name,
                    module_path,
                    class_name,
                )
            else:
                module = import_module(module_path)
                service_class = getattr(module, class_name)
                service = PythonScriptService(service_name, service_class)
            CoronaEditor.register_page(
                service_name,
                service,
            )
            _managed_services.append(service)
            _registered_service_names.add(service_name)
            registered.append(service_name)
            if isinstance(service, LazyPythonScriptService):
                service.start_background_load()
        except Exception:
            logger.exception(
                "Failed to register Python script service %s from %s.%s",
                service_name,
                module_path,
                class_name,
            )
    return registered


def register_python_script_services():
    return _register_python_script_services(PYTHON_SCRIPT_SERVICES)


def register_core_python_script_services():
    return _register_python_script_services(
        name
        for name in PYTHON_SCRIPT_SERVICES
        if name in CORE_PYTHON_SCRIPT_SERVICES
    )


def register_remaining_python_script_services():
    return _register_python_script_services(
        name
        for name in PYTHON_SCRIPT_SERVICES
        if name not in CORE_PYTHON_SCRIPT_SERVICES
    )


def register_active_python_script_services():
    """Register editor services required by the current host.

    Historical Python-only shells remain available through the explicit legacy
    entry point below, but normal startup must not recreate them after the Vue
    panel has moved to the native aggregate contract.
    """

    return _register_python_script_services(
        name
        for name in PYTHON_SCRIPT_SERVICES
        if name not in CORE_PYTHON_SCRIPT_SERVICES
        and name not in LEGACY_PYTHON_SCRIPT_SERVICES
    )


def register_legacy_python_script_services():
    """Explicitly register compatibility services for an old host."""

    return _register_python_script_services(
        name for name in PYTHON_SCRIPT_SERVICES if name in LEGACY_PYTHON_SCRIPT_SERVICES
    )


def shutdown_python_script_services(timeout=2.0):
    _registry_shutdown_requested.set()
    services = list(reversed(_managed_services))
    for service in services:
        try:
            service.request_shutdown()
        except Exception:
            logger.exception("Failed to request shutdown for %s", getattr(service, "module_name", service))

    deadline = time.monotonic() + max(0.0, float(timeout))
    snapshots = []
    for service in services:
        try:
            snapshots.append(service.shutdown(max(0.0, deadline - time.monotonic())))
        except Exception as error:
            logger.exception("Failed to shutdown %s", getattr(service, "module_name", service))
            snapshots.append({
                "service": getattr(service, "module_name", type(service).__name__),
                "state": "error",
                "thread_alive": True,
                "error": str(error),
            })
    _managed_services.clear()
    return snapshots
