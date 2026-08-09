import uuid
from typing import Any, Dict, List, Optional

from runtime.legacy_engine_adapter import CoronaEngine

DEFAULT_VISION_RENDER_MODE = "path_tracing"
VISION_RENDER_MODES = frozenset(("path_tracing", "svgf", "ssat"))
VISION_RENDER_MODE_ALIASES = {
    "pt": "path_tracing",
    "path-tracing": "path_tracing",
    "path tracing": "path_tracing",
    "vision_path_tracing": "path_tracing",
    "vision pt": "path_tracing",
    "vision svgf": "svgf",
    "vision_svgf": "svgf",
    "vision ssat": "ssat",
    "vision_ssat": "ssat",
}


def normalize_vision_render_mode(mode: Optional[str]) -> str:
    value = str(mode or DEFAULT_VISION_RENDER_MODE).strip().lower()
    value = VISION_RENDER_MODE_ALIASES.get(value, value)
    if value not in VISION_RENDER_MODES:
        raise ValueError(
            f"Invalid Vision render mode '{mode}'. "
            f"Expected one of: {', '.join(sorted(VISION_RENDER_MODES))}"
        )
    return value


class Camera:
    """
    OOP 相机包装：统一通过 set(...) 推送到引擎；包装层提供单项 setter 并维护本地缓存。
    包含图像效果、尺寸管理等功能。
    """

    def __init__(self, position: Optional[List[float]] = None, forward: Optional[List[float]] = None,
                 world_up: Optional[List[float]] = None, fov: Optional[float] = None, name: str = "Camera",
                 width: int = 1920, height: int = 1080, camera_id: Optional[str] = None,
                 render_backend: str = "native", output_mode: str = "final_color",
                 vision_render_mode: str = DEFAULT_VISION_RENDER_MODE,
                 move_speed: float = 1.0, view_open: bool = False,
                 view_x: int = 120, view_y: int = 120,
                 view_width: int = 960, view_height: int = 540,
                 ssao_enabled: bool = True,
                 deletable: bool = True):
        if CoronaEngine is None:
            raise RuntimeError("CoronaEngine 未初始化")

        CameraCtor = getattr(CoronaEngine, 'Camera', None)
        if CameraCtor is None:
            raise RuntimeError("CoronaEngine 未提供 Camera 构造器")

        if position is not None and forward is not None and world_up is not None and fov is not None:
            self.engine_obj = CameraCtor(position, forward, world_up, fov)
            self._pos = list(position)
            self._fwd = list(forward)
            self._up = list(world_up)
            self._fov = float(fov)
        else:
            self.engine_obj = CameraCtor()
            self._pos = self.engine_obj.get_position()
            self._fwd = self.engine_obj.get_forward()
            self._up = self.engine_obj.get_world_up()
            self._fov = self.engine_obj.get_fov()

        self.name = name
        self.camera_id = camera_id or str(uuid.uuid4())
        self.width = width
        self.height = height
        self.render_backend = render_backend
        self.output_mode = output_mode
        self.vision_render_mode = normalize_vision_render_mode(vision_render_mode)
        self.shadow_cascade_debug = False
        self.ssao_enabled = bool(ssao_enabled)
        self.move_speed = float(move_speed)
        self.view_open = bool(view_open)
        self.view_x = int(view_x)
        self.view_y = int(view_y)
        self.view_width = int(view_width)
        self.view_height = int(view_height)
        self.deletable = bool(deletable)
        self.engine_obj.set_size(width, height)
        self.engine_obj.set_output_mode(output_mode)
        if hasattr(self.engine_obj, 'set_ssao_enabled'):
            self.engine_obj.set_ssao_enabled(self.ssao_enabled)
        self.engine_obj.set_render_backend(render_backend)
        if hasattr(self.engine_obj, 'set_vision_render_mode'):
            self.engine_obj.set_vision_render_mode(self.vision_render_mode)
        self._flush_view_state()
        # 持有强引用，避免 ImageEffects 被 GC 后底层句柄被释放
        self._image_effects_ref = None

    # 单项 setter：更新缓存并统一调用 set
    def _flush(self):
        self.engine_obj.set(self._pos, self._fwd, self._up, self._fov)

    def set_position(self, position: List[float]):
        self._pos = list(position)
        self._flush()

    def get_position(self) -> List[float]:
        return self.engine_obj.get_position()

    def set_forward(self, forward: List[float]):
        self._fwd = list(forward)
        self._flush()

    def get_forward(self) -> List[float]:
        return self.engine_obj.get_forward()

    def set_world_up(self, world_up: List[float]):
        self._up = list(world_up)
        self._flush()

    def get_world_up(self) -> List[float]:
        return self.engine_obj.get_world_up()

    def set_fov(self, fov: float):
        self._fov = float(fov)
        self._flush()

    def get_fov(self) -> float:
        return self.engine_obj.get_fov()

    # 新接口直通
    def set(self, position: List[float], forward: List[float], world_up: List[float], fov: float):
        self._pos, self._fwd, self._up, self._fov = list(position), list(forward), list(world_up), float(fov)
        self.engine_obj.set(self._pos, self._fwd, self._up, self._fov)

    def get_handle(self) -> int:
        return int(self.engine_obj.get_handle())

    def set_surface(self, surface: int):
        self.engine_obj.set_surface(surface)

    def get_surface(self) -> int:
        return self.engine_obj.get_surface()

    def set_offscreen_capture_mode(self, enabled: bool):
        self.engine_obj.set_offscreen_capture_mode(bool(enabled))
        if enabled:
            self.view_open = False

    def save_screenshot(self, path: str):
        self.engine_obj.save_screenshot(path)

    def save_screenshot_sync(self, path: str):
        self.engine_obj.save_screenshot_sync(path)

    def set_output_mode(self, mode: str):
        self.output_mode = mode
        self.engine_obj.set_output_mode(mode)

    def get_output_mode(self) -> str:
        return self.output_mode

    def set_shadow_cascade_debug(self, enabled: bool):
        self.shadow_cascade_debug = bool(enabled)
        setter = getattr(self.engine_obj, 'set_shadow_cascade_debug', None)
        if callable(setter):
            setter(self.shadow_cascade_debug)

    def get_shadow_cascade_debug(self) -> bool:
        getter = getattr(self.engine_obj, 'get_shadow_cascade_debug', None)
        if callable(getter):
            self.shadow_cascade_debug = bool(getter())
        return bool(getattr(self, 'shadow_cascade_debug', False))

    def set_ssao_enabled(self, enabled: bool):
        self.ssao_enabled = bool(enabled)
        setter = getattr(self.engine_obj, 'set_ssao_enabled', None)
        if callable(setter):
            setter(self.ssao_enabled)

    def get_ssao_enabled(self) -> bool:
        getter = getattr(self.engine_obj, 'get_ssao_enabled', None)
        if callable(getter):
            self.ssao_enabled = bool(getter())
        return bool(getattr(self, 'ssao_enabled', True))

    def set_render_backend(self, mode: str):
        actual = mode
        if mode == "vision" and not CoronaEngine.is_vision_available():
            actual = "native"
        self.engine_obj.set_render_backend(actual)
        self.render_backend = actual

    def get_render_backend(self) -> str:
        return self.render_backend

    def set_vision_render_mode(self, mode: str):
        self.vision_render_mode = normalize_vision_render_mode(mode)
        if hasattr(self.engine_obj, 'set_vision_render_mode'):
            self.engine_obj.set_vision_render_mode(self.vision_render_mode)

    def get_vision_render_mode(self) -> str:
        return self.vision_render_mode

    def _flush_view_state(self):
        self.engine_obj.set_view_state(
            self.view_open, self.view_x, self.view_y,
            self.view_width, self.view_height, self.move_speed)

    def set_view_state(self, open_: bool, x: int, y: int,
                       width: int, height: int, move_speed: Optional[float] = None):
        self.view_open = bool(open_)
        self.view_x = int(x)
        self.view_y = int(y)
        self.view_width = max(int(width), 1)
        self.view_height = max(int(height), 1)
        if move_speed is not None:
            self.move_speed = max(float(move_speed), 0.01)
        self._flush_view_state()

    def refresh_view_state(self):
        return {
            'open': self.view_open,
            'x': self.view_x,
            'y': self.view_y,
            'width': self.view_width,
            'height': self.view_height,
            'move_speed': self.move_speed,
        }

    def refresh_size(self):
        if hasattr(self.engine_obj, 'get_size'):
            size = self.engine_obj.get_size()
            if size and len(size) >= 2:
                self.width = max(int(size[0]), 1)
                self.height = max(int(size[1]), 1)
        return {
            'width': self.width,
            'height': self.height,
        }

    # ========== 图像效果与尺寸管理 ==========
    def set_image_effects(self, effects: Any):
        """设置图像效果"""
        if hasattr(effects, 'engine_obj'):
            self._image_effects_ref = effects
            fx_obj = effects.engine_obj
        else:
            fx_obj = effects
        self.engine_obj.set_image_effects(fx_obj)

    def get_image_effects(self) -> Optional[Any]:
        """获取图像效果"""
        return self._image_effects_ref

    def has_image_effects(self) -> bool:
        """检查是否有图像效果"""
        return self._image_effects_ref is not None

    def remove_image_effects(self):
        """移除图像效果"""
        self._image_effects_ref = None
        self.engine_obj.remove_image_effects()

    def set_size(self, width: int, height: int):
        """设置渲染尺寸"""
        self.width = width
        self.height = height
        self.engine_obj.set_size(width, height)

    def set_viewport_rect(self, x: int, y: int, width: int, height: int):
        """设置视口矩形区域"""
        self.engine_obj.set_viewport_rect(x, y, width, height)

    def pick_actor_at_pixel(self, x: int, y: int):
        """在像素坐标处拾取 Actor"""
        return self.engine_obj.pick_actor_at_pixel(x, y)

    def to_dict(self) -> Dict[str, Any]:
        self.refresh_view_state()
        self.refresh_size()
        return {
            'id': self.camera_id,
            'camera_id': self.camera_id,
            'name': self.name,
            'handle': self.get_handle(),
            'position': list(self.get_position()),
            'forward': list(self.get_forward()),
            'world_up': list(self.get_world_up()),
            'fov': float(self.get_fov()),
            'width': self.width,
            'height': self.height,
            'output_mode': self.get_output_mode(),
            'render_backend': self.get_render_backend(),
            'vision_render_mode': self.get_vision_render_mode(),
            'shadow_cascade_debug': self.get_shadow_cascade_debug(),
            'ssao_enabled': self.get_ssao_enabled(),
            'move_speed': self.move_speed,
            'view_open': self.view_open,
            'view_x': self.view_x,
            'view_y': self.view_y,
            'view_width': self.view_width,
            'view_height': self.view_height,
            'deletable': self.deletable,
        }

    def __repr__(self):
        return f"Camera(name={self.name}, width={self.width}, height={self.height})"
