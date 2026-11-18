import json
import sys
import threading

from runtime.bridge_errors import BridgeError
from runtime.native_engine import get_corona_engine

import logging

logger = logging.getLogger(__name__)


class WebBridgeService:
    """The single Web -> Python entry point.

    C++ transports ``{"service", "function", "args"}`` verbatim: no schema and
    no business branch lives on that side.  Routing, the service allowlist and
    argument handling belong here, where the services themselves live.
    """

    ALLOWED_SERVICES = frozenset({"project_archive", "project_files"})

    @classmethod
    def call(cls, envelope):
        if isinstance(envelope, (bytes, bytearray)):
            envelope = envelope.decode("utf-8")
        if not isinstance(envelope, str):
            raise TypeError("Bridge envelope must be a JSON string")
        request = json.loads(envelope)
        if not isinstance(request, dict):
            raise TypeError("Bridge envelope must be a JSON object")
        service = request.get("service")
        function = request.get("function")
        args = request.get("args", [])
        if not isinstance(service, str) or not service:
            raise ValueError("Bridge request needs a service name")
        if not isinstance(function, str) or not function:
            raise ValueError("Bridge request needs a function name")
        if not isinstance(args, list):
            raise TypeError("Bridge request args must be an array")
        if service not in cls.ALLOWED_SERVICES:
            raise PermissionError(f"Editor Web layer may not call Python service {service}")
        response = CoronaEditor.dispatch_script_values_from_cpp(service, function, args)
        if not response.get("success", False):
            # The dispatcher already turned the failure into a coded refusal, and the
            # message is human text; nothing here parses it.
            raise BridgeError(
                str(response.get("code") or "SERVICE_FAILED"),
                str(response.get("error") or f"{service}.{function} failed"))
        data = response.get("data")
        # ProjectArchive / ProjectFiles answer a refusal in one of two shapes:
        # {status:'error', code, message} (file ops, ProjectFiles) or
        # {success:False, error:<code>, message} (project documents). Raise either one so
        # the bridge rejects with that code instead of resolving a value; the C++ side
        # then hands the same code to the Promise rejection.
        if isinstance(data, dict):
            if data.get("status") == "error":
                raise BridgeError(
                    data.get("code") or "SERVICE_FAILED",
                    data.get("message") or f"{service}.{function} refused the request")
            if data.get("success") is False:
                code = data.get("code") or data.get("error") or "SERVICE_FAILED"
                raise BridgeError(str(code),
                                  str(data.get("message") or f"{service}.{function} refused the request"))
        return data

class CoronaEditor:
    CoronaEngine = get_corona_engine()
    module_list = {}

    _runtime_state = "created"
    _shutdown_requested = False
    _runtime_initialized = False
    _runtime_started = False
    _last_runtime_warning = {}
    _shutdown_snapshot = None
    _runtime_watchdog_error_logged = False
    _runtime_watchdog_file = None

    @classmethod
    def _report_failure(cls, service, function, code, message, *, exc_info=False):
        """One place owns the refusal line: every failure is logged with its code."""
        logger.error(
            "event=bridge.call.failed service=%s function=%s code=%s message=%s",
            service, function, code, str(message or "").replace("\n", " "),
            exc_info=exc_info)

    @classmethod
    def dispatch_script_values_from_cpp(cls, module_name, func_name, args):
        """Invoke a registered service with owned values, without JSON transport.

        Every failure carries a ``code``: the C++ bridge forwards it to the Promise
        rejection, so no caller parses the message and no generic bucket is invented here.
        """
        try:
            if not isinstance(module_name, str) or not module_name:
                raise BridgeError("INVALID_ARGUMENT", "Service name is required")
            if not isinstance(func_name, str) or not func_name:
                raise BridgeError("INVALID_ARGUMENT", "Function name is required")
            if not isinstance(args, (list, tuple)):
                raise BridgeError("INVALID_ARGUMENT", "Service arguments must be an array")
            module = cls.module_list.get(module_name)
            if module is None:
                raise BridgeError("INVALID_ARGUMENT", f"Unknown Python service: {module_name}")
            handler = getattr(module, func_name, None)
            if not callable(handler):
                raise BridgeError(
                    "INVALID_ARGUMENT", f"Unknown function for {module_name}: {func_name}")
            return {"success": True, "data": handler(*args)}
        except BridgeError as error:
            cls._report_failure(module_name, func_name, error.code, error.message)
            return {"success": False, "code": error.code, "error": error.message}
        except Exception as error:
            cls._report_failure(module_name, func_name, "SERVICE_FAILED", str(error),
                                exc_info=True)
            return {"success": False, "code": "SERVICE_FAILED", "error": str(error)}

    @classmethod
    def register_script_dispatcher(cls):
        register = getattr(cls.CoronaEngine, "register_python_script_service_dispatcher", None)
        if callable(register):
            register()
        cls.module_list.setdefault("editor_bridge", WebBridgeService)
        logger.info("event=runtime.dispatcher.registered")

    @classmethod
    def unregister_script_dispatcher(cls):
        unregister = getattr(cls.CoronaEngine, "unregister_python_script_service_dispatcher", None)
        if callable(unregister):
            unregister()
        logger.info("event=runtime.dispatcher.unregistered")

    @classmethod
    def _set_state(cls, state):
        """Record and log one lifecycle transition."""
        cls._runtime_state = state
        logger.info("event=runtime.lifecycle.state state=%s", state)

    @classmethod
    def initialize_runtime(cls):
        if cls._shutdown_requested:
            return False
        if cls._runtime_initialized:
            return True
        cls._set_state("initializing")
        cls._runtime_initialized = True
        cls._set_state("initialized")
        return True

    @classmethod
    def start_runtime(cls):
        if cls._shutdown_requested:
            return False
        if not cls._runtime_initialized:
            cls.initialize_runtime()
        cls._runtime_started = True
        cls._set_state("running")
        return True

    @classmethod
    def request_shutdown(cls):
        cls._shutdown_requested = True
        logger.info("event=runtime.lifecycle.shutdown_requested state=%s", cls._runtime_state)
        if cls._runtime_state not in ("stopped", "stopping"):
            cls._set_state("stop_requested")

    @classmethod
    def checkpoint(cls):
        if cls._shutdown_requested:
            raise RuntimeError("Python runtime shutdown requested")
        return True

    @classmethod
    def _warn_runtime_phase(cls, phase, elapsed_ms, threshold):
        if elapsed_ms < threshold:
            return
        import time as _time
        now = _time.monotonic()
        last = cls._last_runtime_warning.get(phase, 0.0)
        if now - last < 5.0:
            return
        logger.warning(
            "event=python.lifecycle.overrun phase=%s elapsed_ms=%.2f state=%s",
            phase, elapsed_ms, cls._runtime_state)
        cls._last_runtime_warning[phase] = now

    @classmethod
    def shutdown_runtime(cls):
        cls._cancel_runtime_watchdog()
        if cls._runtime_state == "stopped":
            return cls._shutdown_snapshot or {
                "runtime_state": "stopped",
                "services": [],
                "python_threads": [],
            }
        cls.request_shutdown()
        cls._set_state("stopping")
        snapshots = []
        try:
            from runtime import registry as _service_registry
            snapshots = _service_registry.shutdown_python_script_services(2.0)
            alive = [item for item in snapshots if item.get("thread_alive")]
            if alive:
                logger.error("event=runtime.shutdown.services_alive services=%s", alive)
        except Exception:
            logger.exception("event=runtime.shutdown.registry_failed")
        try:
            cls.unregister_script_dispatcher()
        except Exception:
            logger.exception("event=runtime.shutdown.unregister_failed")
        cls.module_list.clear()
        cls._runtime_started = False
        cls._set_state("stopped")
        cls._shutdown_snapshot = {
            "runtime_state": cls._runtime_state,
            "services": snapshots,
            "python_threads": [
                {
                    "name": thread.name,
                    "ident": thread.ident,
                    "daemon": thread.daemon,
                    "alive": thread.is_alive(),
                }
                for thread in threading.enumerate()
            ],
        }
        return cls._shutdown_snapshot

    @classmethod
    def register_page(cls, module_name: str, c_cls: object):
        if module_name not in cls.module_list:
            cls.module_list[module_name] = c_cls

    @classmethod
    def _set_native_runtime_phase(cls, phase):
        try:
            setter = getattr(cls.CoronaEngine, "python_runtime_phase", None)
            if callable(setter):
                setter(str(phase))
        except Exception:
            logger.debug("event=runtime.phase.set_failed phase=%s", phase, exc_info=True)

    @classmethod
    def _cancel_runtime_watchdog(cls):
        import faulthandler

        try:
            if faulthandler.is_enabled():
                faulthandler.cancel_dump_traceback_later()
                faulthandler.disable()
        finally:
            output = cls._runtime_watchdog_file
            cls._runtime_watchdog_file = None
            if output is not None:
                output.flush()
                output.close()

    @classmethod
    def _arm_runtime_watchdog(cls):
        import faulthandler

        if cls._shutdown_requested:
            return
        try:
            if cls._runtime_watchdog_file is None:
                from pathlib import Path

                output_path = Path(sys.executable).resolve().parent / "logs" / "python_faulthandler.log"
                output_path.parent.mkdir(parents=True, exist_ok=True)
                cls._runtime_watchdog_file = output_path.open(
                    "a", encoding="utf-8", buffering=1)
                faulthandler.enable(file=cls._runtime_watchdog_file, all_threads=True)
                logger.info("event=runtime.watchdog.armed path=%s", output_path)
            faulthandler.cancel_dump_traceback_later()
            faulthandler.dump_traceback_later(
                2.0, repeat=False, file=cls._runtime_watchdog_file)
        except Exception:
            output = cls._runtime_watchdog_file
            cls._runtime_watchdog_file = None
            if output is not None:
                output.close()
            if not cls._runtime_watchdog_error_logged:
                logger.exception("event=runtime.watchdog.arm_failed")
                cls._runtime_watchdog_error_logged = True

    @classmethod
    def _update_runtime_impl(cls):
        import time as _time
        runtime_start = _time.perf_counter()
        if cls._shutdown_requested or not cls._runtime_started:
            return False
        cls.checkpoint()
        # Consume SDL and scoped V8 input from the one native owned queue.
        cls._set_native_runtime_phase("input_dispatch")
        input_start = _time.perf_counter()
        try:
            import CoronaEngine
            # The queue belongs to the native side; drain it so it cannot grow while no
            # script host consumes the events.
            CoronaEngine.drain_input_events()
        except RuntimeError:
            return False
        except Exception:
            logger.exception("event=runtime.input.dispatch_failed")
        cls._warn_runtime_phase(
            "input_dispatch", (_time.perf_counter() - input_start) * 1000.0, 50.0)

        elapsed_ms = (_time.perf_counter() - runtime_start) * 1000.0
        cls._warn_runtime_phase("update", elapsed_ms, 50.0)
        return True

    @classmethod
    def update_runtime(cls):
        cls._set_native_runtime_phase("update_entry")
        cls._arm_runtime_watchdog()
        try:
            return cls._update_runtime_impl()
        finally:
            cls._set_native_runtime_phase("idle")
            # Keep the watchdog armed between frames. If another Python thread
            # takes the GIL and blocks in native code, the next C++ update cannot
            # enter Python to arm a new timer; this outstanding timer still dumps
            # every Python thread without requiring the GIL.
            cls._arm_runtime_watchdog()
