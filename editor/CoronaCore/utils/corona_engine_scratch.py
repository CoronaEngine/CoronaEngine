# -*- coding: utf-8 -*-
"""
CoronaEngine Scratch 兼容层。

Blockly 生成的代码仍然按旧方式导入本模块：
    from CoronaCore.utils import corona_engine_scratch as CoronaEngine

运行时状态不再是模块级单例，而是绑定到当前脚本线程的
ScratchRuntimeContext。这样项目预览可以同时运行项目全局脚本和多个
Actor 脚本，而不会互相覆盖目标 Actor、变量或输入处理器。
"""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass, field
import logging
import random as _random
import threading as _threading
import time as _time
from typing import Callable, Optional

_logger = logging.getLogger(__name__)

_engine_lock = _threading.Lock()
_context_lock = _threading.RLock()
_tls = _threading.local()
_run_count = 0


@dataclass
class ScratchRuntimeContext:
    context_id: str
    target_type: str = "actor"  # actor | project | internal
    scene_name: str = ""
    actor_name: str = ""

    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    rot_x: float = 0.0
    rot_y: float = 0.0
    rot_z: float = 0.0
    size_val: float = 100.0
    cartoon_index: int = 0
    visible: bool = True
    variables: dict = field(default_factory=dict)

    target_scene_name: Optional[str] = None
    target_actor_name: Optional[str] = None
    target_scene: object = None
    target_actor: object = None

    geometry: object = None
    optics: object = None
    kinematics: object = None
    mechanics: object = None
    actor: object = None
    scene: object = None
    initialized: bool = False
    external_target: bool = False

    stop_requested: bool = False
    key_state: dict = field(default_factory=dict)
    mouse_pressed: bool = False
    mouse_x: float = 0.0
    mouse_y: float = 0.0
    key_handler: Optional[Callable] = None
    mouse_handler: Optional[Callable] = None


_default_context = ScratchRuntimeContext("default", target_type="internal")
_contexts: dict[str, ScratchRuntimeContext] = {"default": _default_context}


def _current_context() -> ScratchRuntimeContext:
    ctx = getattr(_tls, "ctx", None)
    return ctx if ctx is not None else _default_context


def create_context(
    context_id: str | None = None,
    target_type: str = "actor",
    scene_name: str = "",
    actor_name: str = "",
) -> ScratchRuntimeContext:
    if not context_id:
        context_id = f"scratch-{_threading.get_ident()}-{int(_time.time() * 1000)}"
    ctx = ScratchRuntimeContext(
        context_id=context_id,
        target_type=target_type or "actor",
        scene_name=scene_name or "",
        actor_name=actor_name or "",
    )
    if ctx.target_type == "project":
        ctx.initialized = True
    elif ctx.scene_name and ctx.actor_name:
        ctx.target_scene_name = ctx.scene_name
        ctx.target_actor_name = ctx.actor_name
        ctx.external_target = True
    with _context_lock:
        _contexts[ctx.context_id] = ctx
    return ctx


def bind_context(ctx: ScratchRuntimeContext):
    _tls.ctx = ctx
    with _context_lock:
        _contexts[ctx.context_id] = ctx
    return ctx


def release_context(ctx: ScratchRuntimeContext | None = None):
    if ctx is None:
        ctx = getattr(_tls, "ctx", None)
    if ctx is not None and ctx.context_id != "default":
        with _context_lock:
            _contexts.pop(ctx.context_id, None)
    if getattr(_tls, "ctx", None) is ctx:
        _tls.ctx = None


@contextmanager
def using_context(ctx: ScratchRuntimeContext):
    previous = getattr(_tls, "ctx", None)
    _tls.ctx = ctx
    try:
        yield ctx
    finally:
        _tls.ctx = previous


def _live_contexts() -> list[ScratchRuntimeContext]:
    with _context_lock:
        return [ctx for ctx in _contexts.values() if ctx.context_id != "default"]


def register_key_handler(handler):
    ctx = _current_context()
    ctx.key_handler = handler
    _logger.debug("[ScratchWrapper] key handler registered: %s", getattr(handler, "__name__", None))


def unregister_key_handler():
    _current_context().key_handler = None


def register_mouse_handler(handler):
    _current_context().mouse_handler = handler


def unregister_mouse_handler():
    _current_context().mouse_handler = None


def handle_key_event(key, mods=None, display_key=None):
    if display_key is None:
        display_key = key
    keys_to_set = {key, display_key}
    if len(display_key) == 1:
        keys_to_set.add(display_key.lower())
        keys_to_set.add(display_key.upper())
    for ctx in _live_contexts() or [_default_context]:
        for item in keys_to_set:
            ctx.key_state[item] = True
        if ctx.key_handler is None or ctx.stop_requested:
            continue
        try:
            with using_context(ctx):
                ctx.key_handler(key, mods or [])
        except SystemExit:
            ctx.stop_requested = True
        except Exception as exc:
            _logger.warning("[ScratchWrapper] key handler error: %s", exc)


def handle_key_release(key, display_key=None):
    for ctx in _live_contexts() or [_default_context]:
        ctx.key_state[key] = False
        if display_key:
            ctx.key_state[display_key] = False
            if len(display_key) == 1:
                ctx.key_state[display_key.lower()] = False
                ctx.key_state[display_key.upper()] = False


def handle_mouse_event(event_type, button, x, y):
    for ctx in _live_contexts() or [_default_context]:
        if event_type in ("click", "mousedown"):
            ctx.mouse_pressed = True
        elif event_type == "mouseup":
            ctx.mouse_pressed = False
        ctx.mouse_x = float(x)
        ctx.mouse_y = float(y)
        if ctx.mouse_handler is None or ctx.stop_requested:
            continue
        try:
            with using_context(ctx):
                ctx.mouse_handler(event_type, button, x, y)
        except SystemExit:
            ctx.stop_requested = True
        except Exception as exc:
            _logger.warning("[ScratchWrapper] mouse handler error: %s", exc)


def set_target(scene_name: str, actor_name: str):
    ctx = _current_context()
    ctx.target_type = "actor"
    ctx.scene_name = scene_name or ""
    ctx.actor_name = actor_name or ""
    ctx.target_scene_name = scene_name
    ctx.target_actor_name = actor_name
    ctx.external_target = True
    ctx.initialized = False
    _logger.debug("[ScratchWrapper] set_target: scene=%s actor=%s", scene_name, actor_name)


def set_project_global():
    ctx = _current_context()
    ctx.target_type = "project"
    ctx.external_target = False
    ctx.initialized = True


def _actor_only(api_name: str) -> bool:
    ctx = _current_context()
    if ctx.target_type == "project":
        _logger.warning("[ScratchWrapper] %s ignored in project-global script", api_name)
        return True
    return False


def _init_engine():
    ctx = _current_context()
    if ctx.initialized:
        return
    ctx.initialized = True
    if ctx.target_type == "project":
        return
    if ctx.external_target:
        _init_external_target(ctx)
    else:
        _init_internal_actor(ctx)


def _init_external_target(ctx: ScratchRuntimeContext):
    try:
        from CoronaCore.core.managers import scene_manager

        ctx.target_scene = scene_manager.get(ctx.target_scene_name)
        if ctx.target_scene is None:
            _logger.warning("[ScratchWrapper] scene not found, fallback internal: %s", ctx.target_scene_name)
            _init_internal_actor(ctx)
            return

        ctx.target_actor = ctx.target_scene.find_actor(ctx.target_actor_name)
        if ctx.target_actor is None:
            _logger.warning("[ScratchWrapper] actor not found, fallback internal: %s", ctx.target_actor_name)
            _init_internal_actor(ctx)
            return

        ctx.actor = ctx.target_actor
        ctx.scene = ctx.target_scene
        ctx.geometry = getattr(ctx.target_actor, "_geometry", None)
        ctx.optics = getattr(ctx.target_actor, "_optics", None)
        ctx.kinematics = getattr(ctx.target_actor, "_kinematics", None)
        ctx.mechanics = getattr(ctx.target_actor, "_mechanics", None)

        try:
            pos = ctx.target_actor.get_position()
            ctx.x, ctx.y, ctx.z = float(pos[0]), float(pos[1]), float(pos[2])
        except Exception:
            pass
        try:
            rot = ctx.target_actor.get_rotation()
            ctx.rot_x, ctx.rot_y, ctx.rot_z = float(rot[0]), float(rot[1]), float(rot[2])
        except Exception:
            pass
        try:
            scale = ctx.target_actor.get_scale()
            ctx.size_val = float(scale[0]) * 100.0
        except Exception:
            pass
    except Exception as exc:
        _logger.exception("[ScratchWrapper] bind actor failed: %s", exc)
        _init_internal_actor(ctx)


def _init_internal_actor(ctx: ScratchRuntimeContext):
    ctx.geometry = None
    ctx.optics = None
    ctx.kinematics = None
    ctx.mechanics = None
    ctx.actor = None
    ctx.scene = None


def _sync_position():
    ctx = _current_context()
    check_stop()
    with _engine_lock:
        if ctx.geometry is not None:
            try:
                ctx.geometry.set_position([ctx.x, ctx.y, ctx.z])
                return
            except Exception as exc:
                _logger.debug("_sync_position geometry failed: %s", exc)
        if ctx.actor is not None and hasattr(ctx.actor, "set_position"):
            try:
                ctx.actor.set_position([ctx.x, ctx.y, ctx.z])
            except Exception as exc:
                _logger.debug("_sync_position actor failed: %s", exc)


def _sync_scale():
    ctx = _current_context()
    check_stop()
    scale = ctx.size_val / 100.0
    with _engine_lock:
        if ctx.geometry is not None:
            try:
                ctx.geometry.set_scale([scale, scale, scale])
                return
            except Exception as exc:
                _logger.debug("_sync_scale geometry failed: %s", exc)
        if ctx.actor is not None and hasattr(ctx.actor, "set_scale"):
            try:
                ctx.actor.set_scale([scale, scale, scale])
            except Exception as exc:
                _logger.debug("_sync_scale actor failed: %s", exc)


def _apply_rotation():
    ctx = _current_context()
    check_stop()
    rot = [ctx.rot_x, ctx.rot_y, ctx.rot_z]
    mech_was_enabled = False
    with _engine_lock:
        if ctx.mechanics is not None and hasattr(ctx.mechanics, "set_physics_enabled"):
            try:
                mech_was_enabled = ctx.mechanics.get_physics_enabled()
                if mech_was_enabled:
                    ctx.mechanics.set_physics_enabled(False)
            except Exception:
                pass
        for target, method in (
            (ctx.kinematics, "set_rotation"),
            (ctx.geometry, "set_rotation"),
            (ctx.actor, "set_rotation"),
        ):
            if target is not None and hasattr(target, method):
                try:
                    getattr(target, method)(rot)
                except Exception as exc:
                    _logger.debug("_apply_rotation %s failed: %s", method, exc)
        if mech_was_enabled and ctx.mechanics is not None:
            try:
                ctx.mechanics.set_physics_enabled(True)
            except Exception:
                pass


# Engine / motion
def move(steps):
    if _actor_only("move"):
        return
    _init_engine()
    ctx = _current_context()
    ctx.x += float(steps)
    _sync_position()


def rotateX(angle):
    if _actor_only("rotateX"):
        return
    _init_engine()
    ctx = _current_context()
    ctx.rot_x += float(angle)
    if ctx.kinematics is not None and hasattr(ctx.kinematics, "rotate_x"):
        try:
            ctx.kinematics.rotate_x(float(angle))
        except Exception:
            pass
    _apply_rotation()


def rotateY(angle):
    if _actor_only("rotateY"):
        return
    _init_engine()
    ctx = _current_context()
    ctx.rot_y += float(angle)
    if ctx.kinematics is not None and hasattr(ctx.kinematics, "rotate_y"):
        try:
            ctx.kinematics.rotate_y(float(angle))
        except Exception:
            pass
    _apply_rotation()


def rotateZ(angle):
    if _actor_only("rotateZ"):
        return
    _init_engine()
    ctx = _current_context()
    ctx.rot_z += float(angle)
    if ctx.kinematics is not None and hasattr(ctx.kinematics, "rotate_z"):
        try:
            ctx.kinematics.rotate_z(float(angle))
        except Exception:
            pass
    _apply_rotation()


def face(direction):
    if _actor_only("face"):
        return
    _init_engine()
    _current_context().rot_y = float(direction)
    _apply_rotation()


def rotationX():
    _init_engine()
    return _current_context().rot_x


def rotationY():
    _init_engine()
    return _current_context().rot_y


def rotationZ():
    _init_engine()
    return _current_context().rot_z


def moveto(position):
    if _actor_only("moveto"):
        return
    _init_engine()
    ctx = _current_context()
    if position == "random_position":
        ctx.x = _random.uniform(-10, 10)
        ctx.y = _random.uniform(-5, 5)
        ctx.z = _random.uniform(-10, 10)
    elif position == "sight_position":
        ctx.x, ctx.y, ctx.z = 0.0, 0.0, 0.0
    else:
        _logger.warning("[ScratchWrapper] unknown moveto position: %s", position)
        return
    _sync_position()


def movetoXYZ(position):
    _logger.debug("[ScratchWrapper] movetoXYZ(%s)", position)


def movetoXYZtime(t, x1, x2, x3):
    if _actor_only("movetoXYZtime"):
        return
    _init_engine()
    ctx = _current_context()
    ctx.x, ctx.y, ctx.z = float(x1), float(x2), float(x3)
    _sync_position()


def Xset(x):
    if _actor_only("Xset"):
        return
    _init_engine()
    _current_context().x = float(x)
    _sync_position()


def Yset(y):
    if _actor_only("Yset"):
        return
    _init_engine()
    _current_context().y = float(y)
    _sync_position()


def Zset(z):
    if _actor_only("Zset"):
        return
    _init_engine()
    _current_context().z = float(z)
    _sync_position()


def Xadd(dx):
    if _actor_only("Xadd"):
        return
    _init_engine()
    _current_context().x += float(dx)
    _sync_position()


def Yadd(dy):
    if _actor_only("Yadd"):
        return
    _init_engine()
    _current_context().y += float(dy)
    _sync_position()


def Zadd(dz):
    if _actor_only("Zadd"):
        return
    _init_engine()
    _current_context().z += float(dz)
    _sync_position()


def X():
    _init_engine()
    return _current_context().x


def Y():
    _init_engine()
    return _current_context().y


def Z():
    _init_engine()
    return _current_context().z


# ── Camera / FPS controls ──

def lock_mouse():
    """锁定鼠标到窗口中心，隐藏光标，启用 FPS 相对模式"""
    try:
        import corona_engine as _ce
        if hasattr(_ce, 'set_mouse_locked'):
            _ce.set_mouse_locked(True)
            return
    except Exception:
        pass
    _logger.debug("[ScratchWrapper] lock_mouse: engine API not available")


def unlock_mouse():
    """解锁鼠标，显示光标"""
    try:
        import corona_engine as _ce
        if hasattr(_ce, 'set_mouse_locked'):
            _ce.set_mouse_locked(False)
            return
    except Exception:
        pass
    _logger.debug("[ScratchWrapper] unlock_mouse: engine API not available")


def mouse_dx():
    """获取本帧鼠标 X 位移量"""
    try:
        import corona_engine as _ce
        if hasattr(_ce, 'get_mouse_delta'):
            dx, _dy = _ce.get_mouse_delta()
            return float(dx)
    except Exception:
        pass
    return 0.0


def mouse_dy():
    """获取本帧鼠标 Y 位移量"""
    try:
        import corona_engine as _ce
        if hasattr(_ce, 'get_mouse_delta'):
            _dx, dy = _ce.get_mouse_delta()
            return float(dy)
    except Exception:
        pass
    return 0.0


def set_fov(fov_degrees):
    """设置摄像机视场角（用于瞄准镜缩放效果）"""
    try:
        import corona_engine as _ce
        if hasattr(_ce, 'Camera'):
            cam = _ce.Camera()
            if hasattr(cam, 'set_fov'):
                cam.set_fov(float(fov_degrees))
                return
    except Exception:
        pass
    _logger.debug("[ScratchWrapper] set_fov: engine API not available")


# ── Physics extension (velocity / impulse) ──

_velocity_cache = {}  # context_id → [vx, vy, vz]


def set_velocity(vx, vy, vz):
    """设置当前 Actor 的线速度"""
    if _actor_only("set_velocity"):
        return
    _init_engine()
    ctx = _current_context()
    vel = [float(vx), float(vy), float(vz)]
    _velocity_cache[ctx.context_id] = list(vel)
    for target in (ctx.mechanics, ctx.actor):
        if target is not None and hasattr(target, 'set_velocity'):
            try:
                target.set_velocity(vel)
                return
            except Exception:
                pass
    # fallback: 每帧偏移位置模拟速度（假设 ~60fps）
    ctx.x += float(vx) * 0.016
    ctx.y += float(vy) * 0.016
    ctx.z += float(vz) * 0.016
    _sync_position()


def apply_impulse(ix, iy, iz):
    """施加瞬时冲量（子弹命中反馈）"""
    if _actor_only("apply_impulse"):
        return
    _init_engine()
    ctx = _current_context()
    imp = [float(ix), float(iy), float(iz)]
    for target in (ctx.mechanics, ctx.actor):
        if target is not None and hasattr(target, 'apply_impulse'):
            try:
                target.apply_impulse(imp)
                return
            except Exception:
                pass
    # fallback: 直接偏移位置
    ctx.x += float(ix) * 0.1
    ctx.y += float(iy) * 0.1
    ctx.z += float(iz) * 0.1
    _sync_position()


def get_velocity(axis='X'):
    """获取当前速度分量"""
    cache = _velocity_cache.get(_current_context().context_id, [0.0, 0.0, 0.0])
    mapping = {'X': 0, 'Y': 1, 'Z': 2}
    return cache.get(mapping.get(axis, 0), 0.0)


# Appearance
def cartoonSet(index):
    if _actor_only("cartoonSet"):
        return
    _init_engine()
    ctx = _current_context()
    ctx.cartoon_index = int(index)
    if ctx.kinematics is not None:
        try:
            ctx.kinematics.set_animation(ctx.cartoon_index)
        except Exception:
            pass


def nextCartoon():
    if _actor_only("nextCartoon"):
        return
    _init_engine()
    ctx = _current_context()
    ctx.cartoon_index += 1
    if ctx.kinematics is not None:
        try:
            ctx.kinematics.set_animation(ctx.cartoon_index)
        except Exception:
            pass


def playCartoon():
    if _actor_only("playCartoon"):
        return
    _init_engine()
    ctx = _current_context()
    if ctx.kinematics is not None:
        try:
            ctx.kinematics.play_animation()
        except Exception:
            pass


def stopCartoon():
    if _actor_only("stopCartoon"):
        return
    _init_engine()
    ctx = _current_context()
    if ctx.kinematics is not None:
        try:
            ctx.kinematics.stop_animation()
        except Exception:
            pass


def resetCartoon():
    if _actor_only("resetCartoon"):
        return
    _init_engine()
    ctx = _current_context()
    ctx.cartoon_index = 0
    if ctx.kinematics is not None:
        try:
            ctx.kinematics.set_animation(0)
        except Exception:
            pass


def sizeAdd(ds):
    if _actor_only("sizeAdd"):
        return
    _init_engine()
    _current_context().size_val += float(ds)
    _sync_scale()


def sizeSet(sz):
    if _actor_only("sizeSet"):
        return
    _init_engine()
    _current_context().size_val = float(sz)
    _sync_scale()


def show(v=None):
    if _actor_only("show"):
        return
    _init_engine()
    ctx = _current_context()
    ctx.visible = True
    for target in (ctx.optics, ctx.actor):
        if target is not None and hasattr(target, "set_visible"):
            try:
                target.set_visible(True)
                return
            except Exception:
                pass


def hide(v=None):
    if _actor_only("hide"):
        return
    _init_engine()
    ctx = _current_context()
    ctx.visible = False
    for target in (ctx.optics, ctx.actor):
        if target is not None and hasattr(target, "set_visible"):
            try:
                target.set_visible(False)
                return
            except Exception:
                pass


def cartoon():
    _init_engine()
    return _current_context().cartoon_index


def size():
    _init_engine()
    return _current_context().size_val


def set_color(r, g, b):
    """设置物体漫反射颜色（Disney Principled BRDF diffuse）"""
    if _actor_only("set_color"):
        return
    _init_engine()
    ctx = _current_context()
    color = [float(r), float(g), float(b)]
    for target in (ctx.optics, ctx.actor):
        if target is not None and hasattr(target, 'set_diffuse'):
            try:
                target.set_diffuse(color)
                return
            except Exception:
                pass
    _logger.debug("[ScratchWrapper] set_color: no optics target available")


def set_alpha(alpha):
    """设置物体透明度（1.0=不透明，0.0=全透明）"""
    if _actor_only("set_alpha"):
        return
    _init_engine()
    ctx = _current_context()
    a = float(alpha)
    for target in (ctx.optics, ctx.actor):
        if target is not None and hasattr(target, 'set_opacity'):
            try:
                target.set_opacity(a)
                return
            except Exception:
                pass
    _logger.debug("[ScratchWrapper] set_alpha: engine API not available")


# Detect
def update_key_state(key, pressed):
    _current_context().key_state[key] = bool(pressed)


def update_mouse_state(pressed, x, y):
    ctx = _current_context()
    ctx.mouse_pressed = bool(pressed)
    ctx.mouse_x = float(x)
    ctx.mouse_y = float(y)


def touch(target):
    return False


def distance(target):
    return 0.0


def ask(question):
    _logger.info("[ScratchWrapper] ask: %s", question)
    try:
        return input(question)
    except (EOFError, OSError):
        return ""


def keyboard(key):
    return _current_context().key_state.get(key, False)


def keyboard0(key):
    return not _current_context().key_state.get(key, False)


def mouse1():
    return _current_context().mouse_pressed


def mouse0():
    return not _current_context().mouse_pressed


def attribute(name):
    _init_engine()
    ctx = _current_context()
    values = {
        "X": ctx.x,
        "Y": ctx.y,
        "Z": ctx.z,
        "SIZE": ctx.size_val,
        "DIRECTION": ctx.rot_y,
        "ROTX": ctx.rot_x,
        "ROTY": ctx.rot_y,
        "ROTZ": ctx.rot_z,
        "NAME": ctx.actor_name or ctx.context_id,
        "ID": ctx.context_id,
    }
    return values.get(name, 0.0)


# ── Raycast (射击命中检测) ──

_raycast_cache = {}  # context_id → {hit, distance, object, point}


def _get_raycast_cache():
    """获取当前上下文的射线检测结果缓存"""
    ctx_id = _current_context().context_id
    if ctx_id not in _raycast_cache:
        _raycast_cache[ctx_id] = {
            'hit': False, 'distance': 0.0, 'object': '', 'point': (0.0, 0.0, 0.0)
        }
    return _raycast_cache[ctx_id]


def raycast_hit(origin, direction, max_dist=100.0):
    """执行射线检测，返回是否命中。结果缓存供 raycast_distance 等查询。"""
    cache = _get_raycast_cache()
    try:
        import corona_engine as _ce
        if hasattr(_ce, 'ray_cast'):
            result = _ce.ray_cast(origin, direction, float(max_dist))
            cache['hit'] = bool(result.hit)
            if result.hit:
                cache['distance'] = float(result.distance)
                cache['object'] = str(getattr(result, 'handle', ''))
                cache['point'] = (
                    float(result.point[0]) if result.point else 0.0,
                    float(result.point[1]) if result.point else 0.0,
                    float(result.point[2]) if result.point else 0.0,
                )
            else:
                cache['distance'] = float(max_dist)
                cache['object'] = ''
                cache['point'] = (0.0, 0.0, 0.0)
            return cache['hit']
    except Exception as exc:
        _logger.debug("[ScratchWrapper] raycast engine fallback: %s", exc)

    # Fallback: 手动 AABB 遍历当前场景所有 Actor
    cache['hit'] = False
    cache['distance'] = float(max_dist)
    cache['object'] = ''
    cache['point'] = (0.0, 0.0, 0.0)
    ctx = _current_context()
    if ctx.scene is not None and hasattr(ctx.scene, 'get_all_actors'):
        try:
            actors = ctx.scene.get_all_actors()
            origin_vec = [float(origin[0]), float(origin[1]), float(origin[2])]
            dir_vec = [float(direction[0]), float(direction[1]), float(direction[2])]
            # 归一化方向
            import math as _math
            mag = _math.sqrt(dir_vec[0]**2 + dir_vec[1]**2 + dir_vec[2]**2)
            if mag > 0:
                dir_vec = [d / mag for d in dir_vec]
            closest = float(max_dist)
            for actor in actors:
                if not hasattr(actor, 'get_aabb'):
                    continue
                try:
                    aabb = actor.get_aabb()
                except Exception:
                    continue
                t = _ray_aabb_intersect(origin_vec, dir_vec, aabb)
                if t is not None and 0 < t < closest:
                    closest = t
                    cache['hit'] = True
                    cache['distance'] = closest
                    cache['object'] = str(getattr(actor, 'name', ''))
                    cache['point'] = (
                        origin_vec[0] + dir_vec[0] * closest,
                        origin_vec[1] + dir_vec[1] * closest,
                        origin_vec[2] + dir_vec[2] * closest,
                    )
        except Exception as exc:
            _logger.debug("[ScratchWrapper] raycast fallback failed: %s", exc)
    return cache['hit']


def _ray_aabb_intersect(origin, direction, aabb):
    """射线-AABB 相交检测（slab 方法），返回 t 或 None"""
    t_min = float('-inf')
    t_max = float('inf')
    for i in range(3):
        if abs(direction[i]) < 1e-10:
            if origin[i] < aabb[i] or origin[i] > aabb[i + 3]:
                return None
        else:
            t1 = (aabb[i] - origin[i]) / direction[i]
            t2 = (aabb[i + 3] - origin[i]) / direction[i]
            if t1 > t2:
                t1, t2 = t2, t1
            t_min = max(t_min, t1)
            t_max = min(t_max, t2)
    if t_min > t_max or t_max < 0:
        return None
    return t_min if t_min >= 0 else t_max


def raycast_distance():
    """获取最近一次射线检测的命中距离"""
    return _get_raycast_cache()['distance']


def raycast_hit_object():
    """获取最近一次射线检测命中的物体标识"""
    return _get_raycast_cache()['object']


def raycast_hit_point_x():
    """获取命中点 X 坐标"""
    return float(_get_raycast_cache()['point'][0])


def raycast_hit_point_y():
    """获取命中点 Y 坐标"""
    return float(_get_raycast_cache()['point'][1])


def raycast_hit_point_z():
    """获取命中点 Z 坐标"""
    return float(_get_raycast_cache()['point'][2])


# Control
def check_stop():
    if _current_context().stop_requested:
        raise SystemExit(0)


def wait(seconds):
    remaining = float(seconds)
    while remaining > 0:
        check_stop()
        step = min(0.1, remaining)
        _time.sleep(step)
        remaining -= step


def stop(option):
    if option in ("ALL_SCRIPTS", "all"):
        request_stop_all()
    raise SystemExit(0)


def cloneStart():
    _logger.debug("[ScratchWrapper] cloneStart")


def clone(name):
    _logger.debug("[ScratchWrapper] clone(%s)", name)


def deleteClone():
    _logger.debug("[ScratchWrapper] deleteClone")


def setScene(name):
    _logger.debug("[ScratchWrapper] setScene(%s)", name)


def nextScene():
    _logger.debug("[ScratchWrapper] nextScene")


# Event
def gameStart():
    _logger.debug("[ScratchWrapper] gameStart")


def RB(message):
    _logger.debug("[ScratchWrapper] RB: %s", message)


def broadcast(message):
    _logger.debug("[ScratchWrapper] broadcast: %s", message)


def broadcastWait(message):
    _logger.debug("[ScratchWrapper] broadcastWait: %s", message)


# Math / variables / lists
def random(a, b):
    return _random.uniform(float(a), float(b))


def var_add(name, value):
    ctx = _current_context()
    ctx.variables[name] = ctx.variables.get(name, 0.0) + float(value)


def var_set(name, value):
    _current_context().variables[name] = float(value)


def var_show(name):
    ctx = _current_context()
    _logger.info("[ScratchWrapper] var_show: %s = %s", name, ctx.variables.get(name, 0.0))


def var_hide(name):
    _logger.debug("[ScratchWrapper] var_hide: %s", name)


def list_show(name):
    _logger.debug("[ScratchWrapper] list_show: %s", name)


def list_hide(name):
    _logger.debug("[ScratchWrapper] list_hide: %s", name)


# ── Audio ──

_audio_cache = {}  # name → resource_id (str)


def _ensure_audio(name):
    """确保音效已加载到引擎，返回 resource_id；失败返回 None"""
    if name in _audio_cache:
        return _audio_cache[name]
    import os as _os
    # 从 CoronaCore/utils 向上两级到项目根，再进入 assets/audio
    audio_dir = _os.path.join(
        _os.path.dirname(_os.path.abspath(__file__)), '..', '..', 'assets', 'audio'
    )
    for ext in ('.wav', '.mp3', '.ogg'):
        path = _os.path.join(audio_dir, name + ext)
        if _os.path.exists(path):
            try:
                import corona_engine as _ce
                media = _ce.import_media(path)
                _audio_cache[name] = media.resource_id
                _logger.debug("[ScratchWrapper] loaded audio: %s → %s", name, media.resource_id)
                return media.resource_id
            except Exception as exc:
                _logger.warning("[ScratchWrapper] audio load failed: %s %s", name, exc)
                return None
    # 如果文件不存在，尝试直接用名称作为 resource_id（引擎可能已预加载）
    try:
        import corona_engine as _ce
        _audio_cache[name] = name
        return name
    except Exception:
        pass
    _logger.debug("[ScratchWrapper] audio not found: %s", name)
    return None


def play_sound(name):
    """播放音效（单次）"""
    rid = _ensure_audio(name)
    if rid is not None:
        try:
            import corona_engine as _ce
            _ce.play_audio(rid, loop=False)
            _logger.debug("[ScratchWrapper] play_sound: %s", name)
        except Exception as exc:
            _logger.warning("[ScratchWrapper] play_sound failed: %s", exc)


def loop_sound(name):
    """循环播放音效（BGM 用）"""
    rid = _ensure_audio(name)
    if rid is not None:
        try:
            import corona_engine as _ce
            _ce.play_audio(rid, loop=True)
            _logger.debug("[ScratchWrapper] loop_sound: %s", name)
        except Exception as exc:
            _logger.warning("[ScratchWrapper] loop_sound failed: %s", exc)


def stop_sound(name):
    """停止指定音效"""
    rid = _audio_cache.get(name)
    if rid is not None:
        try:
            import corona_engine as _ce
            _ce.stop_audio(rid)
        except Exception:
            pass


def stop_all_sounds():
    """停止所有已加载音效"""
    import corona_engine as _ce
    for name, rid in list(_audio_cache.items()):
        if rid is not None:
            try:
                _ce.stop_audio(rid)
            except Exception:
                pass
    _audio_cache.clear()


# Stop/reset compatibility
def reset_state():
    global _run_count
    ctx = _current_context()
    fresh = ScratchRuntimeContext(ctx.context_id, target_type=ctx.target_type)
    fresh.scene_name = ctx.scene_name
    fresh.actor_name = ctx.actor_name
    if fresh.target_type == "actor" and fresh.scene_name and fresh.actor_name:
        fresh.target_scene_name = fresh.scene_name
        fresh.target_actor_name = fresh.actor_name
        fresh.external_target = True
    if fresh.target_type == "project":
        fresh.initialized = True
    _run_count += 1
    bind_context(fresh)


def request_stop(context_id: str | None = None):
    if context_id:
        with _context_lock:
            ctx = _contexts.get(context_id)
        if ctx is not None:
            ctx.stop_requested = True
        return

    ctx = getattr(_tls, "ctx", None)
    if ctx is not None and ctx.context_id != "default":
        ctx.stop_requested = True
        return

    request_stop_all()


def request_stop_all():
    with _context_lock:
        for ctx in _contexts.values():
            ctx.stop_requested = True


def reset_stop():
    _current_context().stop_requested = False


def is_stop_requested():
    return _current_context().stop_requested


def active_context_count():
    return len(_live_contexts())
