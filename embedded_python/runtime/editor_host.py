import json
import sys
import threading

from runtime.bridge_errors import BridgeError
from runtime.native_engine import get_corona_engine
from config.paths_config import get_default_paths
from runtime.response_utils import create_error_response, create_success_response

core_path = get_default_paths()

import logging

logger = logging.getLogger(__name__)


class WebBridgeService:
    """The single Web -> Python entry point.

    C++ transports ``{"service", "function", "args"}`` verbatim: no schema and
    no business branch lives on that side.  Routing, the service allowlist and
    argument handling belong here, where the services themselves live.
    """

    ALLOWED_SERVICES = frozenset({"ProjectArchive", "FileScripts", "ProjectFiles"})

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
            message = response.get("error") or f"{service}.{function} failed"
            # A service refusal already carries "CODE: message"; re-raise it as a typed
            # error so the outer dispatcher keeps the code instead of prefixing
            # "Error processing request: " in front of it.
            code, separator, detail = message.partition(": ")
            if separator and code.isupper() and all(ch.isalnum() or ch == "_" for ch in code):
                raise BridgeError(code, detail)
            raise RuntimeError(message)
        data = response.get("data")
        # ProjectArchive / ProjectFiles answer a refusal in one of two shapes:
        # {status:'error', code, message} (file ops, ProjectFiles) or
        # {success:False, error:<code>, message} (project documents). Raise either one so
        # the bridge rejects with that code instead of resolving a value; the C++ side
        # then hands the same code to the Promise rejection. FileScripts is deliberately
        # excluded: its replies are run-state records whose `status` is the state the
        # Blockly panel renders, not a transport refusal.
        if service != "FileScripts" and isinstance(data, dict):
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
    url = core_path.frontend_dist
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
    def dispatch_script_values_from_cpp(cls, module_name, func_name, args):
        """Invoke the existing service with owned values, without JSON transport."""
        try:
            if not isinstance(module_name, str) or not module_name or not isinstance(func_name, str) or not func_name:
                raise ValueError("Please input script module and function")
            if not isinstance(args, (list, tuple)):
                raise TypeError("Script service arguments must be an array")
            module = cls.module_list.get(module_name)
            if module is None or not hasattr(module, func_name):
                raise ValueError("Not find script function")
            result = getattr(module, func_name)(*args)
            return {"success": True, "data": result}
        except BridgeError as error:
            # Keep the business code in front of the message: the C++ bridge extracts it
            # and rejects with the code rather than flattening every refusal to
            # SERVICE_FAILED.
            return {"success": False, "error": f"{error.code}: {error.message}"}
        except Exception as error:
            return {"success": False, "error": f"Error processing request: {error}"}

    @classmethod
    def _dispatch_script_request(cls, json_str):
        # Retain the non-Web text adapter; both forms use the same service owner.
        try:
            request = json.loads(json_str)
            if 'api' in request:
                return create_error_response("Editor API payload is not accepted by Python script service dispatcher")
            response = cls.dispatch_script_values_from_cpp(
                request.get('module'), request.get('function'), request.get('args', []))
            if response["success"]:
                return create_success_response(response["data"])
            return create_error_response(response["error"])
        except json.JSONDecodeError as error:
            return create_error_response(f"Invalid JSON: {error}")
        except Exception as error:
            return create_error_response(f"Error processing request: {error}")

    @classmethod
    def dispatch_script_request_from_cpp(cls, json_str):
        """Compatibility adapter for existing non-Web dispatcher registrations."""
        return cls._dispatch_script_request(json_str)

    @classmethod
    def register_script_dispatcher(cls):
        register = getattr(cls.CoronaEngine, "register_python_script_service_dispatcher", None)
        if callable(register):
            register(cls.dispatch_script_request_from_cpp)
        cls.module_list.setdefault("EditorBridge", WebBridgeService)

    @classmethod
    def unregister_script_dispatcher(cls):
        unregister = getattr(cls.CoronaEngine, "unregister_python_script_service_dispatcher", None)
        if callable(unregister):
            unregister()

    @classmethod
    def initialize_runtime(cls):
        if cls._shutdown_requested:
            return False
        if cls._runtime_initialized:
            return True
        cls._runtime_state = "initializing"
        cls._runtime_initialized = True
        cls._runtime_state = "initialized"
        return True

    @classmethod
    def start_runtime(cls):
        if cls._shutdown_requested:
            return False
        if not cls._runtime_initialized:
            cls.initialize_runtime()
        cls._runtime_started = True
        cls._runtime_state = "running"
        return True

    @classmethod
    def is_shutting_down(cls):
        return cls._shutdown_requested

    @classmethod
    def request_shutdown(cls):
        cls._shutdown_requested = True
        if cls._runtime_state not in ("stopped", "stopping"):
            cls._runtime_state = "stop_requested"

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
            "python.lifecycle.overrun phase=%s elapsed_ms=%.2f state=%s",
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
        cls._runtime_state = "stopping"
        snapshots = []
        try:
            from runtime import registry as _service_registry
            snapshots = _service_registry.shutdown_python_script_services(2.0)
            alive = [item for item in snapshots if item.get("thread_alive")]
            if alive:
                logger.error("Python services still alive during shutdown: %s", alive)
        except Exception:
            logger.exception("Python service registry shutdown failed")
        try:
            cls.unregister_script_dispatcher()
        except Exception:
            logger.exception("Python dispatcher unregister failed")
        cls.module_list.clear()
        cls._runtime_started = False
        cls._runtime_state = "stopped"
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

    _editor_camera_input_locks = set()
    _editor_camera_input_state_lock = threading.RLock()

    @classmethod
    def set_editor_camera_input_enabled(cls, enabled, reason="global"):
        reason_key = str(reason or "global")
        # Status RPCs can arrive on different bridge threads. Keep the reason set,
        # aggregate value and native setter ordered as one operation so an older
        # disable call cannot run after the final unlock.
        with cls._editor_camera_input_state_lock:
            if enabled:
                cls._editor_camera_input_locks.discard(reason_key)
            else:
                cls._editor_camera_input_locks.add(reason_key)
            input_enabled = not cls._editor_camera_input_locks
            # Always re-apply the aggregate state. The native input gate can be
            # recreated while this long-lived Python class still remembers the old
            # value (for example after switching projects in the same editor process).
            try:
                import CoronaEngine
                setter = getattr(CoronaEngine, "camera_follow_set_input_enabled", None)
                if setter is not None:
                    setter(input_enabled)
            except Exception:
                logger.debug("CameraFollowController input gate unavailable", exc_info=True)

    @classmethod
    def _set_native_runtime_phase(cls, phase):
        try:
            setter = getattr(cls.CoronaEngine, "python_runtime_phase", None)
            if callable(setter):
                setter(str(phase))
        except Exception:
            logger.debug("Unable to update native Python runtime phase", exc_info=True)

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
                logger.info("Python runtime watchdog output: %s", output_path)
            faulthandler.cancel_dump_traceback_later()
            faulthandler.dump_traceback_later(
                2.0, repeat=False, file=cls._runtime_watchdog_file)
        except Exception:
            output = cls._runtime_watchdog_file
            cls._runtime_watchdog_file = None
            if output is not None:
                output.close()
            if not cls._runtime_watchdog_error_logged:
                logger.exception("Unable to arm Python runtime watchdog")
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
            events = CoronaEngine.drain_input_events()
            if events:
                from script_runtime.file_runner import FileScripts
                for e in events:
                    if e.type == 0:      # keyDown
                        FileScripts.key_event(e.arg0, e.arg1, e.arg2)
                    elif e.type == 1:    # keyUp
                        FileScripts.key_release(e.arg0, e.arg1 or e.arg0)
                    elif e.type == 2:    # mouseEvent
                        FileScripts.mouse_event(e.arg0, e.arg1, e.arg3, e.arg4,
                                                e.viewport_x, e.viewport_y, e.viewport_width, e.viewport_height, e.picked_actor)
        except RuntimeError:
            return False
        except Exception:
            logger.exception("Input event dispatch failed")
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


def set_editor_camera_input_enabled(enabled, *, reason="global"):
    """Canonical runtime helper for the aggregated camera-input gate."""
    return CoronaEditor.set_editor_camera_input_enabled(enabled, reason=reason)
