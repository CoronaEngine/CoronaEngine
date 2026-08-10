import json
import os
import sys
import threading

from runtime.native_engine import get_corona_engine
from config.paths_config import get_default_paths
from config.project_state import settings_manager
from runtime.response_utils import *

core_path = get_default_paths()

import logging

logger = logging.getLogger(__name__)

class CoronaEditor:
    CoronaEngine = get_corona_engine()
    url = core_path.frontend_dist
    module_list = {}

    _selected_scene = None
    _selected_actor = None
    _runtime_state = "created"
    _shutdown_requested = False
    _runtime_initialized = False
    _runtime_started = False
    _last_runtime_warning = {}
    _shutdown_snapshot = None
    _runtime_watchdog_error_logged = False
    _runtime_watchdog_file = None

    @classmethod
    def _dispatch_script_request(cls, json_str):
        try:
            request = json.loads(json_str)
            if 'api' in request:
                return create_error_response(
                    "Editor API payload is not accepted by Python script service dispatcher"
                )
            module_name = request.get('module', None)
            func_name = request.get('function', None)
            args = request.get('args', [])
            log_level = logging.DEBUG  # 全量降为 DEBUG，默认静默
            logger.log(log_level, f"script request: {module_name}.{func_name} args: {args}")
            if not module_name or not func_name:
                return create_error_response("Please input script module and function")

            if module_name not in cls.module_list or not hasattr(cls.module_list[module_name], func_name):
                return create_error_response("Not find script function")

            module = cls.module_list.get(module_name, None)
            result = getattr(module, func_name)(*args)
            return create_success_response(result)
        except json.JSONDecodeError as e:
            return create_error_response(f"Invalid JSON: {str(e)}")
        except Exception as e:
            return create_error_response(f"Error processing request: {str(e)}")

    @classmethod
    def dispatch_script_request_from_cpp(cls, json_str):
        """C++ 脚本服务显式调用 Python 运行时时使用的内部入口。"""
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
    def update_project_context(cls, project_path):
        """Apply the native launcher's authoritative project on the Python thread."""
        normalized_path = os.path.abspath(os.path.expanduser(str(project_path or "").strip()))
        if not normalized_path or not settings_manager.set_active_project(normalized_path):
            return False
        if cls.CoronaEngine is not None:
            cls.CoronaEngine.active_project_path = normalized_path
        return True

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
        if cls.scripts_mgr is not None:
            try:
                cls.scripts_mgr.shutdown()
            except Exception:
                logger.exception("ScriptsManager shutdown failed")
            cls.scripts_mgr = None
        cls._scripts_initialized = False
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
    def emit_editor_event(cls, event_name, args=None):
        """把历史 Python 脚本事件收口到 C++ 定义的 Editor API 事件。"""
        if args is None:
            args = []
        args = list(args) if isinstance(args, (list, tuple)) else [args]
        logger.debug("Python editor event emitted: %s args=%s", event_name, args)

        event_mapping = {
            "ai-chunk": ("events.on_ai_chunk", lambda values: values[0] if len(values) > 0 else ""),
            "log-batch": ("events.on_log_batch", lambda values: values[0] if len(values) > 0 else []),
            "scene-add": ("events.on_scene_added", lambda values: {
                "name": values[0] if len(values) > 0 else "",
                "route": values[1] if len(values) > 1 else "",
            }),
            "scene-rename": ("events.on_scene_renamed", lambda values: {
                "old_path": values[0] if len(values) > 0 else "",
                "new_path": values[1] if len(values) > 1 else "",
                "name": values[2] if len(values) > 2 else "",
            }),
            "scene-tree-changed": ("events.on_scene_tree_changed", lambda values: {
                "scene": values[0] if len(values) > 0 else "",
            }),
            "actor-change": ("events.on_actor_changed", lambda values: {
                "actor_type": values[0] if len(values) > 0 else "",
                "scene": values[1] if len(values) > 1 else "",
                "actor": values[2] if len(values) > 2 else "",
                "previous": values[3] if len(values) > 3 else "",
            }),
            "transform-update": ("events.on_actor_transform_updated", lambda values: {
                "scene": values[0] if len(values) > 0 else "",
                "actor": values[1] if len(values) > 1 else "",
                "position": values[2] if len(values) > 2 else {},
                "rotation": values[3] if len(values) > 3 else {},
                "scale": values[4] if len(values) > 4 else {},
                "actor_type": values[5] if len(values) > 5 else "",
            }),
            "actor-ownership-claim": ("events.on_network_actor_ownership_claimed", lambda values: {
                "actor_guid": (values[0] if len(values) > 0 else {}).get("actor_guid", "")
                if isinstance(values[0] if len(values) > 0 else {}, dict)
                else "",
            }),
            "actor-sync-broadcast": ("events.on_network_actor_sync_broadcast_requested", lambda values: values[0] if len(values) > 0 else {}),
            "actor-transform-sync-broadcast": ("events.on_network_actor_transform_sync_broadcast_requested", lambda values: values[0] if len(values) > 0 else {}),
            "actor-state-sync-broadcast": ("events.on_network_actor_state_sync_broadcast_requested", lambda values: values[0] if len(values) > 0 else {}),
            "actor-delete-sync-broadcast": ("events.on_network_actor_delete_sync_broadcast_requested", lambda values: values[0] if len(values) > 0 else {}),
            "network-sync-pause-request": ("events.on_network_sync_pause_requested", lambda values: {
                "paused": bool((values[0] if len(values) > 0 else {}).get("paused", False))
            }),
            "file-sync-status": ("events.on_network_file_sync_status_changed", lambda values: {
                "status": (values[0] if len(values) > 0 else {}).get("status", ""),
                "model_path": (values[0] if len(values) > 0 else {}).get("model_path", ""),
                "progress": (values[0] if len(values) > 0 else {}).get("progress", 0),
            }),
            "import-asset-complete": ("events.on_network_asset_import_completed", lambda values: values[0] if len(values) > 0 else {}),
        }
        mapped_event = event_mapping.get(event_name)
        if mapped_event:
            event_wrapper, payload_factory = mapped_event
            from api.editor_api import _emit_manifest_cpp_editor_api_event
            _emit_manifest_cpp_editor_api_event(event_wrapper, payload_factory(args))
        return f"Editor event emitted: {event_name}"

    @classmethod
    def register_page(cls, module_name: str, c_cls: object):
        if module_name not in cls.module_list:
            cls.module_list[module_name] = c_cls

    @classmethod
    def close_process(cls) -> None:
        """请求引擎优雅退出（与点击 SDL 窗口关闭按钮走完全相同的路径）"""
        import CoronaEngine
        CoronaEngine.request_engine_exit()

    # ================================================================
    # 摄像机跟随
    # ================================================================

    _camera_follow_actor = None
    _camera_follow_scene = None
    _camera_follow_offset = [0.0, 0.0, 2.0]
    _held_keys = set()
    _editor_camera_input_enabled = True
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
            cls._editor_camera_input_enabled = input_enabled
            if not input_enabled:
                cls._held_keys.clear()
                cls._follow_rmb_down = False
                cls._follow_prev_mouse = None
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
    def camera_lock_set(cls, enabled, ox=0.0, oy=0.0, oz=2.0, rx=0.0, ry=0.0, rz=0.0):
        if not enabled:
            try:
                if cls._selected_scene and cls._selected_actor:
                    from api.editor_api import CoronaEditorApi
                    result = CoronaEditorApi.scene_tools.set_actor_camera_lock(
                        cls._selected_scene,
                        cls._selected_actor,
                        {"enabled": False},
                    )
                    if isinstance(result, dict) and result.get("status") != "error":
                        cls._camera_follow_actor = None
                        cls._camera_follow_scene = None
                        cls._held_keys.clear()
                        logger.debug("Camera follow disabled through sceneTools contract")
                        return {"ok": True}
            except Exception:
                logger.debug("Native camera-lock disable unavailable", exc_info=True)
            cls._camera_follow_actor = None
            cls._camera_follow_scene = None
            cls._held_keys.clear()
            logger.debug("Camera follow disabled")
            return {"ok": True}
        scene_name = cls._selected_scene
        actor_name = cls._selected_actor
        if not scene_name and not actor_name:
            return {"ok": False, "error": "请先在Object面板选中一个物体"}
        try:
            from api.editor_api import CoronaEditorApi
            if not scene_name:
                snapshot = CoronaEditorApi.scene.get_snapshot()
                if not isinstance(snapshot, dict) or snapshot.get("status") == "error":
                    return {"ok": False, "error": f"未找到物体: {actor_name}"}
                scene_name = snapshot.get("scene", "")
                actors = snapshot.get("actors", [])
                if not scene_name or not any(
                    isinstance(actor, dict) and actor.get("name") == actor_name
                    for actor in actors
                ):
                    return {"ok": False, "error": f"未找到物体: {actor_name}"}
            camera_lock = {"enabled": True}
            if (cls._camera_follow_actor == actor_name and
                    (ox != 0.0 or oy != 0.0 or oz != 2.0)):
                camera_lock["position_offset"] = [ox, oy, oz]
            if rx != 0.0 or ry != 0.0 or rz != 0.0:
                camera_lock["rotation_offset"] = [rx, ry, rz]

            result = CoronaEditorApi.scene_tools.set_actor_camera_lock(
                scene_name, actor_name, camera_lock
            )
            if not isinstance(result, dict) or result.get("status") == "error":
                return {
                    "ok": False,
                    "error": (result.get("message", f"未找到物体: {actor_name}")
                              if isinstance(result, dict)
                              else f"未找到物体: {actor_name}"),
                }
            actor_data = result.get("actor", {})
            native_lock = actor_data.get("camera_lock", {}) if isinstance(actor_data, dict) else {}
            cls._camera_follow_offset = native_lock.get(
                "position_offset", [ox, oy, oz]
            )
            cls._camera_follow_actor = actor_name
            cls._camera_follow_scene = scene_name
            cls._follow_debug_once = True
            logger.info("Camera following %s (offset=%s)", actor_name, cls._camera_follow_offset)
            return {"ok": True, "offset": cls._camera_follow_offset}
        except Exception as e:
            logger.error("camera_lock_set failed: %s", e)
            return {"ok": False, "error": str(e)}

    @classmethod
    def object_key_down(cls, key):
        if not cls._editor_camera_input_enabled:
            return {"ok": True}
        cls._held_keys.add(key.lower())
        return {"ok": True}

    @classmethod
    def object_key_up(cls, key):
        if not cls._editor_camera_input_enabled:
            return {"ok": True}
        cls._held_keys.discard(key.lower())
        return {"ok": True}

    # 鼠标右键环绕相关
    _follow_rmb_down = False
    _follow_prev_mouse = None
    _follow_orbit_sensitivity = 0.004
    _follow_cam_look_at = True

    _follow_frame_count = 0
    _follow_logged_init = False

    scripts_mgr = None
    _scripts_initialized = False

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
        if not cls._scripts_initialized and cls.CoronaEngine is not None:
            cls._set_native_runtime_phase("script_initialize")
            init_start = _time.perf_counter()
            try:
                project_path = getattr(cls.CoronaEngine, 'active_project_path', None)
                if not project_path:
                    project_path = settings_manager.active_project_path

                if project_path:
                    from script_runtime.engine.host import initialize_scripts

                    initialize_scripts(cls, project_path)
            except Exception:
                logger.exception("ScriptsManager initialization failed")
            cls._warn_runtime_phase(
                "script_initialize", (_time.perf_counter() - init_start) * 1000.0, 500.0)

        cls.checkpoint()
        if cls.scripts_mgr is not None:
            cls._set_native_runtime_phase("script_update")
            update_start = _time.perf_counter()
            try:
                import time as _time
                now = _time.perf_counter()
                delta = now - getattr(cls, '_last_script_update', now)
                cls._last_script_update = now
                cls.scripts_mgr.update(min(delta, 0.1))
            except RuntimeError:
                return False
            except Exception:
                logger.exception("Script runtime update failed")
            cls._warn_runtime_phase(
                "script_update", (_time.perf_counter() - update_start) * 1000.0, 50.0)

        # ── Input 事件队列消费：CEF InputInject → 队列 → Python─ ─
        # 每帧批量消费积攒的键盘/鼠标注入事件，消除逐事件 cefQuery 开销
        cls._set_native_runtime_phase("input_dispatch")
        input_start = _time.perf_counter()
        try:
            import CoronaEngine
            events = CoronaEngine.drain_input_events()
            if events:
                from script_runtime.engine import corona_engine as corona_engine_scratch
                for e in events:
                    if e.type == 0:      # keyDown
                        corona_engine_scratch.handle_key_event(e.arg0, e.arg1.split(',') if e.arg1 else [], e.arg2)
                    elif e.type == 1:    # keyUp
                        corona_engine_scratch.handle_key_release(e.arg0, e.arg1 or e.arg0)
                    elif e.type == 2:    # mouseEvent
                        corona_engine_scratch.handle_mouse_event(e.arg0, e.arg1, e.arg3, e.arg4)
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

    @classmethod
    def show_log_on_js(cls):
        """Backward-compatible C++ entry point for one runtime update."""
        if not cls._runtime_initialized:
            cls.initialize_runtime()
        if not cls._runtime_started:
            cls.start_runtime()
        return cls.update_runtime()


def emit_editor_event(event_name, args=None):
    """Canonical runtime helper for the historical editor-event vocabulary."""
    return CoronaEditor.emit_editor_event(event_name, args)


def set_editor_camera_input_enabled(enabled, *, reason="global"):
    """Canonical runtime helper for the aggregated camera-input gate."""
    return CoronaEditor.set_editor_camera_input_enabled(enabled, reason=reason)


def get_editor_selection():
    """Return the current host selection as a value tuple."""
    return CoronaEditor._selected_scene, CoronaEditor._selected_actor
