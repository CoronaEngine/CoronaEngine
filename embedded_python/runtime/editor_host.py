import json
import sys
import threading

from runtime.native_engine import get_corona_engine
from config.paths_config import get_default_paths
from runtime.response_utils import create_error_response, create_success_response

core_path = get_default_paths()

import logging

logger = logging.getLogger(__name__)

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
