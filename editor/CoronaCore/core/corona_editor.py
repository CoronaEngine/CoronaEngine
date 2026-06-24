import json
import math
import os
import sys

from CoronaCore.core.corona_engine import get_corona_engine
from utils.settings import core_path
from CoronaCore.utils.response_utils import *

import logging

logger = logging.getLogger(__name__)

# 高频 UI 心跳类回调，日志降到 DEBUG，避免淹没业务日志
_NOISY_FUNCTIONS = frozenset({
    "on_init",
})


class CoronaEditor:
    CoronaEngine = get_corona_engine()
    url = core_path.frontend_dist
    module_list = {}

    _selected_scene = None
    _selected_actor = None
    _main_tab_id = None  # 主 CEF Tab ID（由 main.py 设置）

    @classmethod
    def deal_func_from_js(cls, json_str):
        try:
            request = json.loads(json_str)
            module_name = request.get('module', None)
            func_name = request.get('function', None)
            args = request.get('args', [])
            log_level = logging.DEBUG  # 全量降为 DEBUG，默认静默
            logger.log(log_level, f"func_name: {func_name} module_name: {module_name} args: {args}")
            if not module_name or not func_name:
                return create_error_response(f"Please input module and function")

            if module_name not in cls.module_list or not hasattr(cls.module_list[module_name], func_name):
                return create_error_response(f"Not find module or function")

            module = cls.module_list.get(module_name, None)
            result = getattr(module, func_name)(*args)
            return create_success_response(result)
        except json.JSONDecodeError as e:
            return create_error_response(f"Invalid JSON: {str(e)}")
        except Exception as e:
            return create_error_response(f"Error processing request: {str(e)}")

    @classmethod
    def js_call_func(cls, event_name, args=None):
        """Python -> Vue 推送：通过 window.__coronaEmit 向主 Tab 发送事件"""
        if cls.CoronaEngine and cls._main_tab_id is not None:
            try:
                if args is None:
                    args = []
                args_str = ', '.join(
                    json.dumps(arg, ensure_ascii=False)
                    for arg in args
                )
                js_code = f"""
                    if (window.__coronaEmit) {{
                        window.__coronaEmit('{event_name}', {args_str});
                    }}
                """
                cls.CoronaEngine.execute_javascript(cls._main_tab_id, js_code)
                result = f"Emitted event '{event_name}' with args: {args_str}"
                return result
            except Exception as e:
                return f"Failed to emit event: {str(e)}"
        else:
            return f"CoronaEngine not available. Would emit: {event_name}"

    @classmethod
    def start_corona_engine(cls):
        """Vue 接管面板管理，Python 仅发送引擎就绪事件"""
        cls.js_call_func("engine-started", [])

    @classmethod
    def register_page(cls, module_name: str, c_cls: object):
        if module_name not in cls.module_list:
            cls.module_list[module_name] = c_cls

    @classmethod
    def reload_frontend(cls):
        """强制刷新主浏览器标签页"""
        if cls.CoronaEngine and cls._main_tab_id is not None:
            try:
                cls.CoronaEngine.execute_javascript(cls._main_tab_id, "location.reload(true)")
            except Exception:
                pass
            return "Frontend reloaded"
        return "CoronaEngine not available"

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

    @classmethod
    def set_editor_camera_input_enabled(cls, enabled, reason="global"):
        reason_key = str(reason or "global")
        if enabled:
            cls._editor_camera_input_locks.discard(reason_key)
        else:
            cls._editor_camera_input_locks.add(reason_key)
        input_enabled = not cls._editor_camera_input_locks
        previous = cls._editor_camera_input_enabled
        cls._editor_camera_input_enabled = input_enabled
        if not input_enabled:
            cls._held_keys.clear()
            cls._follow_rmb_down = False
            cls._follow_prev_mouse = None
        if previous == input_enabled:
            return
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
            # 清除 C++ 侧 CameraFollowController 目标
            try:
                import CoronaEngine
                CoronaEngine.camera_follow_clear()
            except Exception:
                pass
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
            from CoronaCore.core.managers import scene_manager
            if scene_name:
                scene = scene_manager.get(scene_name)
                actor = scene.find_actor(actor_name) if scene else None
            else:
                scene = None
                actor = scene_manager.find_actor(actor_name)
            if actor is None:
                return {"ok": False, "error": f"未找到物体: {actor_name}"}
            if scene:
                cam = scene.get_active_camera()
            else:
                cam = None
                for s_name in scene_manager.list_all():
                    s = scene_manager.get(s_name)
                    if s:
                        cam = s.get_active_camera()
                        if cam:
                            break
            if cam is None:
                return {"ok": False, "error": "未找到摄像机"}

            # 计算 offset
            cam_pos = cam.get_position()
            obj_pos = actor.get_position()
            world_offset = [
                cam_pos[0] - obj_pos[0],
                cam_pos[1] - obj_pos[1],
                cam_pos[2] - obj_pos[2],
            ]
            if cls._camera_follow_actor == actor_name and (ox != 0.0 or oy != 0.0 or oz != 2.0):
                cls._camera_follow_offset = [ox, oy, oz]
            else:
                cls._camera_follow_offset = world_offset
            cls._camera_follow_actor = actor_name
            cls._camera_follow_scene = scene_name

            # 设置 C++ 侧 CameraFollowController 目标
            try:
                import CoronaEngine
                actor_h = getattr(actor.engine_obj, 'get_handle', lambda: 0)()
                cam_h = cam.get_handle()
                if actor_h and cam_h:
                    CoronaEngine.camera_follow_set_target(actor_h, cam_h,
                        cls._camera_follow_offset[0],
                        cls._camera_follow_offset[1],
                        cls._camera_follow_offset[2])
            except Exception as e:
                logger.warning("CameraFollowController set_target failed: %s", e)
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

    @classmethod
    def _update_camera_follow(cls):
        cls._follow_frame_count += 1
        if not cls._follow_logged_init:
            cls._follow_logged_init = True
            logger.debug("[CAMFOLLOW] _update_camera_follow is being called")
        if not cls._camera_follow_actor:
            return
        if cls._follow_frame_count % 60 == 0:
            logger.debug("[CAMFOLLOW] actor=%s held_keys=%s offset=%s", cls._camera_follow_actor, cls._held_keys, cls._camera_follow_offset)
        try:
            from CoronaCore.core.managers import scene_manager
            actor = None
            scene = None
            if cls._camera_follow_scene:
                scene = scene_manager.get(cls._camera_follow_scene)
                if scene:
                    actor = scene.find_actor(cls._camera_follow_actor)
            if actor is None:
                actor = scene_manager.find_actor(cls._camera_follow_actor)
            if actor is None:
                return
            if scene:
                cam = scene.get_active_camera()
            else:
                cam = None
                for s_name in scene_manager.list_all():
                    s = scene_manager.get(s_name)
                    if s:
                        cam = s.get_active_camera()
                        if cam: break
            if cam is None:
                return
            obj_pos = actor.get_position()
            input_enabled = cls._editor_camera_input_enabled

            w_down = a_down = s_down = d_down = 0
            if input_enabled:
                try:
                    import ctypes
                    w_down = ctypes.windll.user32.GetAsyncKeyState(0x57) & 0x8000
                    a_down = ctypes.windll.user32.GetAsyncKeyState(0x41) & 0x8000
                    s_down = ctypes.windll.user32.GetAsyncKeyState(0x53) & 0x8000
                    d_down = ctypes.windll.user32.GetAsyncKeyState(0x44) & 0x8000
                except Exception:
                    pass
                for k in list(cls._held_keys):
                    if k == 'w': w_down = 0x8000
                    elif k == 'a': a_down = 0x8000
                    elif k == 's': s_down = 0x8000
                    elif k == 'd': d_down = 0x8000

                if w_down or a_down or s_down or d_down:
                    # 从 offset 推断相机朝向，确保 WASD 方向与观察方向一致
                    ox, oy, oz = cls._camera_follow_offset
                    look_dir = cls._normalize([-ox, -oy, -oz])
                    fwd_xz = cls._normalize([look_dir[0], 0.0, look_dir[2]])
                    right_xz = cls._normalize(cls._cross([0.0, 1.0, 0.0], fwd_xz))
                    move = [0.0, 0.0, 0.0]
                    step = 0.5
                    if w_down: move[0] += fwd_xz[0] * step; move[2] += fwd_xz[2] * step
                    if s_down: move[0] -= fwd_xz[0] * step; move[2] -= fwd_xz[2] * step
                    if a_down: move[0] -= right_xz[0] * step; move[2] -= right_xz[2] * step
                    if d_down: move[0] += right_xz[0] * step; move[2] += right_xz[2] * step
                    obj_pos = [obj_pos[0] + move[0], obj_pos[1] + move[1], obj_pos[2] + move[2]]
                    actor.set_position(obj_pos, if_init=True)
                    logger.debug("[CAMFOLLOW] WASD move to %s", obj_pos)

                # 鼠标右键拖动物体
                rmb_down = False
                try:
                    rmb_down = ctypes.windll.user32.GetAsyncKeyState(0x02) & 0x8000
                except Exception:
                    pass

                if rmb_down:
                    cur_mouse = None
                    try:
                        class POINT(ctypes.Structure):
                            _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]
                        pt = POINT()
                        ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
                        cur_mouse = (pt.x, pt.y)
                    except Exception:
                        pass

                    if not cls._follow_rmb_down:
                        cls._follow_rmb_down = True
                        cls._follow_prev_mouse = cur_mouse
                    else:
                        if cur_mouse and cls._follow_prev_mouse:
                            dx = cur_mouse[0] - cls._follow_prev_mouse[0]
                            dy = cur_mouse[1] - cls._follow_prev_mouse[1]
                            cls._follow_prev_mouse = cur_mouse
                            if dx != 0 or dy != 0:
                                ox, oy, oz = cls._camera_follow_offset
                                look_dir = cls._normalize([-ox, -oy, -oz])
                                fwd_xz = cls._normalize([look_dir[0], 0.0, look_dir[2]])
                                right_xz = cls._normalize(cls._cross([0.0, 1.0, 0.0], fwd_xz))
                                rmb_speed = 0.02
                                move = [0.0, 0.0, 0.0]
                                if dx != 0:
                                    move[0] += right_xz[0] * dx * rmb_speed
                                    move[2] += right_xz[2] * dx * rmb_speed
                                if dy != 0:
                                    move[0] += fwd_xz[0] * (-dy) * rmb_speed
                                    move[2] += fwd_xz[2] * (-dy) * rmb_speed
                                obj_pos = [obj_pos[0] + move[0], obj_pos[1] + move[1], obj_pos[2] + move[2]]
                                actor.set_position(obj_pos, if_init=True)
                                logger.debug("[CAMFOLLOW] RMB move to %s", obj_pos)
                else:
                    cls._follow_rmb_down = False
                    cls._follow_prev_mouse = None
            else:
                cls._follow_rmb_down = False
                cls._follow_prev_mouse = None

            # 摄像机跟随（位置 + 注视）
            ox, oy, oz = cls._camera_follow_offset
            cam.set_position([obj_pos[0] + ox, obj_pos[1] + oy, obj_pos[2] + oz])

            # 让摄像机始终注视物体
            if cls._follow_cam_look_at:
                look_dir = cls._normalize([-ox, -oy, -oz])
                cam.set_forward(look_dir)
                cam.set_world_up([0.0, 1.0, 0.0])

        except Exception as e:
            logger.error("[CAMFOLLOW] error: %s", e)

    scripts_mgr = None
    _scripts_initialized = False

    @staticmethod
    def _normalize(v):
        length = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
        if length < 1e-10:
            return [0.0, 0.0, 1.0]
        return [v[0] / length, v[1] / length, v[2] / length]

    @staticmethod
    def _cross(a, b):
        return [
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0],
        ]

    @classmethod
    def show_log_on_js(cls):
        if not cls._scripts_initialized and cls.CoronaEngine is not None:
            try:
                project_path = getattr(cls.CoronaEngine, 'active_project_path', None)
                if not project_path:
                    from utils.settings import settings_manager as _sm
                    project_path = _sm.active_project_path

                if project_path:
                    from CoronaCore.core.managers import scene_manager
                    scenes = scene_manager.list_all()
                    if scenes:
                        from CoronaCore.core.scripts_system.scripts_manager import ScriptsManager
                        import os as _os
                        if cls.scripts_mgr is None:
                            cls.scripts_mgr = ScriptsManager()
                        project_script = _os.path.join(project_path, 'Scripts', 'project_script.py')
                        scene = scene_manager.get(scenes[0])
                        if scene:
                            cls.scripts_mgr.initialize_project(project_script, scene)
                            logger.debug("ScriptsManager: 懒初始化完成，场景=%s", scene.name)
                        cls._scripts_initialized = True
            except Exception:
                pass

        if cls.scripts_mgr is not None:
            try:
                import time as _time
                now = _time.perf_counter()
                delta = now - getattr(cls, '_last_script_update', now)
                cls._last_script_update = now
                cls.scripts_mgr.update(min(delta, 0.1))
            except Exception:
                pass

        # ── Input 事件队列消费：CEF InputInject → 队列 → Python─ ─
        # 每帧批量消费积攒的键盘/鼠标注入事件，消除逐事件 cefQuery 开销
        try:
            import CoronaEngine
            events = CoronaEngine.drain_input_events()
            if events:
                from CoronaCore.utils import corona_engine_scratch
                for e in events:
                    if e.type == 0:      # keyDown
                        corona_engine_scratch.handle_key_event(e.arg0, e.arg1.split(',') if e.arg1 else [], e.arg2)
                    elif e.type == 1:    # keyUp
                        corona_engine_scratch.handle_key_release(e.arg0, e.arg1 or e.arg0)
                    elif e.type == 2:    # mouseEvent
                        corona_engine_scratch.handle_mouse_event(e.arg0, e.arg1, e.arg3, e.arg4)
        except Exception:
            pass

        if "LogTool" in cls.module_list and cls.CoronaEngine:
            cls.module_list["LogTool"].show_log()
        return True
