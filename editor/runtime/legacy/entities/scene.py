import configparser
import base64
import json
import logging
import os
from pathlib import Path
from typing import List, Optional, Dict, Any
import zlib
from .actor import Actor
from .environment import Environment

from api.editor_api import (
    emit_compat_editor_event,
    get_active_project_path,
    get_scene_adapter,
    get_scene_tools_adapter,
    get_viewport_adapter,
    is_native_engine_available,
)
from runtime.scene_support import auto_save
from runtime.archive.parser import parse_archive

logger = logging.getLogger(__name__)
VISION_DOCUMENT_ENCODING = "zlib_base64_json"
VISION_DOCUMENT_VERSION = "1"


def _active_project_path():
    return get_active_project_path()


def _format_float(value) -> str:
    return format(float(value), ".17g")


def _format_float3(values) -> str:
    return ", ".join(_format_float(values[index]) for index in range(3))


def _encode_vision_document(document: Dict[str, Any]) -> str:
    payload = json.dumps(document, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    compressor = zlib.compressobj(level=0)
    compressed = compressor.compress(payload) + compressor.flush()
    return base64.b64encode(compressed).decode('ascii')


def _decode_vision_document(data: str) -> Dict[str, Any]:
    payload = zlib.decompress(base64.b64decode(data.encode('ascii'))).decode('utf-8')
    document = json.loads(payload)
    if not isinstance(document, dict):
        raise ValueError("Vision document must decode to a JSON object")
    return document


class NativeSceneRecord:
    """Python metadata proxy for the C++ native editor scene."""

    def __init__(self, route: str):
        self.route = route
        self._enabled = True
        self._simulation_enabled = True

    def set_environment(self, environment):
        return None

    def set_sun_direction(self, direction):
        return None

    def set_enabled(self, enabled: bool):
        self._enabled = bool(enabled)

    def is_enabled(self) -> bool:
        return self._enabled

    def set_simulation_enabled(self, enabled: bool):
        self._simulation_enabled = bool(enabled)

    def is_simulation_enabled(self) -> bool:
        return self._simulation_enabled

    def get_aabb(self):
        adapter = get_scene_adapter()
        getter = getattr(adapter, "get_snapshot", None) if adapter is not None else None
        if callable(getter):
            try:
                raw = getter(self.route)
                result = json.loads(raw) if isinstance(raw, str) else raw
                if isinstance(result, dict):
                    for key in ("scene_aabb", "aabb"):
                        aabb = result.get(key)
                        if isinstance(aabb, list) and len(aabb) >= 6:
                            return aabb[:6]
            except Exception as exc:
                logger.debug("NativeSceneRecord get_aabb failed scene=%s: %s", self.route, exc)
        return [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]


class NativeCameraRecord:
    """Python metadata view for a C++ native editor camera."""

    def __init__(self, scene_route: str, name: str, camera_id: str = "",
                 position=None, forward=None, world_up=None, fov: float = 45.0,
                 width: int = 1920, height: int = 1080,
                 render_backend: str = "native", output_mode: str = "final_color",
                 vision_render_mode: str = "path_tracing", move_speed: float = 1.0,
                 view_open: bool = False, view_x: int = 120, view_y: int = 120,
                 view_width: int = 960, view_height: int = 540,
                 deletable: bool = True):
        self.scene_route = scene_route
        self.name = name
        self.camera_id = camera_id or f"{scene_route}#{name}"
        self._pos = list(position or [0.0, 0.0, -5.0])
        self._fwd = list(forward or [0.0, 0.0, 1.0])
        self._up = list(world_up or [0.0, 1.0, 0.0])
        self._fov = float(fov)
        self.width = int(width)
        self.height = int(height)
        self.render_backend = render_backend or "native"
        self.output_mode = output_mode or "final_color"
        self.vision_render_mode = vision_render_mode or "path_tracing"
        self.move_speed = float(move_speed)
        self.view_open = bool(view_open)
        self.view_x = int(view_x)
        self.view_y = int(view_y)
        self.view_width = int(view_width)
        self.view_height = int(view_height)
        self.deletable = bool(deletable)

    def set(self, position, forward, world_up, fov: float):
        self._pos = list(position)
        self._fwd = list(forward)
        self._up = list(world_up)
        self._fov = float(fov)

    def get_position(self):
        return list(self._pos)

    def get_forward(self):
        return list(self._fwd)

    def get_world_up(self):
        return list(self._up)

    def get_fov(self):
        return self._fov

    def set_output_mode(self, mode: str):
        self.output_mode = mode or self.output_mode

    def get_output_mode(self) -> str:
        return self.output_mode

    def set_render_backend(self, mode: str):
        self.render_backend = mode or self.render_backend

    def get_render_backend(self) -> str:
        return self.render_backend

    def set_vision_render_mode(self, mode: str):
        if mode:
            self.vision_render_mode = mode

    def get_vision_render_mode(self) -> str:
        return self.vision_render_mode

    def set_size(self, width: int, height: int):
        self.width = max(int(width), 1)
        self.height = max(int(height), 1)

    def refresh_size(self):
        return {"width": self.width, "height": self.height}

    def set_view_state(self, open_: bool, x: int, y: int,
                       width: int, height: int, move_speed: Optional[float] = None):
        self.view_open = bool(open_)
        self.view_x = int(x)
        self.view_y = int(y)
        self.view_width = max(int(width), 1)
        self.view_height = max(int(height), 1)
        if move_speed is not None:
            self.move_speed = max(float(move_speed), 0.01)

    def refresh_view_state(self):
        return {
            "open": self.view_open,
            "x": self.view_x,
            "y": self.view_y,
            "width": self.view_width,
            "height": self.view_height,
            "move_speed": self.move_speed,
        }

    def set_surface(self, surface: int):
        return None

    def save_screenshot_sync(self, path: str):
        adapter = get_viewport_adapter()
        capture = getattr(adapter, "capture", None) if adapter is not None else None
        if not callable(capture):
            return False
        payload = {
            "position": self._pos,
            "forward": self._fwd,
            "world_up": self._up,
            "fov": self._fov,
            "width": self.width,
            "height": self.height,
            "output_mode": self.output_mode,
            "render_backend": self.render_backend,
            "vision_render_mode": self.vision_render_mode,
        }
        try:
            raw = capture(self.scene_route, self.name, payload, path)
            result = json.loads(raw) if isinstance(raw, str) else raw
            return isinstance(result, dict) and result.get("status", "success") in ("success", "ok")
        except Exception as exc:
            logger.warning("NativeCameraRecord screenshot failed scene=%s camera=%s: %s",
                           self.scene_route, self.name, exc)
            return False

    def save_screenshot(self, path: str):
        return self.save_screenshot_sync(path)

    def to_dict(self):
        return {
            "id": self.camera_id,
            "camera_id": self.camera_id,
            "name": self.name,
            "position": self.get_position(),
            "forward": self.get_forward(),
            "world_up": self.get_world_up(),
            "fov": self.get_fov(),
            "width": self.width,
            "height": self.height,
            "output_mode": self.output_mode,
            "render_backend": self.render_backend,
            "vision_render_mode": self.vision_render_mode,
            "move_speed": self.move_speed,
            "view_open": self.view_open,
            "view_x": self.view_x,
            "view_y": self.view_y,
            "view_width": self.view_width,
            "view_height": self.view_height,
            "deletable": self.deletable,
        }


class Scene:
    """
    场景包装类：仅管理对象引用，生命周期由外部管理。
    Python 层只保留 .scene 持久化与兼容查询，运行态由 C++ native scene 管理。
    """

    def __init__(self, route):

        self.route = route
        self.name = Path(route).stem
        self.file_data = configparser.ConfigParser()
        self.archive_snapshot: Dict[str, Any] = {}

        if not is_native_engine_available():
            raise RuntimeError("CoronaEngine 未初始化")
        self.engine_scene = NativeSceneRecord(self.route)

        # 引用列表（Python 层）
        self._actors: List[Actor] = []
        self._cameras: List[NativeCameraRecord] = []
        self._main_camera: Optional[NativeCameraRecord] = None

        # 环境对象（Python 层）- 自动创建默认环境
        self._environment: Optional[Environment] = None
        try:
            self._environment = Environment(name=f"{self.name}_Environment")
            self.set_environment(self._environment, True)
        except Exception as e:
            # 如果创建失败，继续运行但记录警告
            import logging
            logging.warning(f"Failed to create Environment for Scene '{self.name}': {e}")

        self.terrain_type = ''
        self.terrain_path = ''
        self.script_path = ''
        self.vision_storage = ''
        self.vision_source_id = ''
        self.vision_source_path = ''
        self.vision_import_mode = ''
        self.vision_document: Optional[Dict[str, Any]] = None
        self.vision_document_encoding = VISION_DOCUMENT_ENCODING
        self.vision_document_asset_root = ''
        self.vision_bindings: List[Dict[str, Any]] = []
        self.vision_unsupported_shapes: List[Dict[str, Any]] = []

        self.read_data()
        # 应用保存的相机数据
        self._apply_pending_camera_data()

        # 默认启用物理模拟（否则 MechanicsSystem 会跳过该场景的所有物理计算）
        self.set_simulation_enabled(True)

    @auto_save
    def set_route(self, route):
        self.route = route
        self.name = Path(route).stem
        # save_data 会由装饰器自动调用
        return True

    def _is_portable_scene_folder(self) -> bool:
        return (
            self.file_data.get('format', 'type', fallback='') == 'corona_scene_folder'
            and self.file_data.getint('format', 'version', fallback=0) == 1
        )

    def read_data(self):
        previous_enabled = self._begin_bulk_scene_load()
        try:
            self._read_data_unchecked()
        finally:
            self._end_bulk_scene_load(previous_enabled)

    def _begin_bulk_scene_load(self) -> Optional[bool]:
        engine_scene = getattr(self, 'engine_scene', None)
        if engine_scene is None or not hasattr(engine_scene, 'set_enabled'):
            return None

        was_enabled = True
        if hasattr(engine_scene, 'is_enabled'):
            try:
                was_enabled = bool(engine_scene.is_enabled())
            except Exception as exc:
                logger.warning("Scene '%s': failed to query enabled state before load: %s",
                               getattr(self, 'name', ''), exc)

        if not was_enabled:
            return None

        try:
            engine_scene.set_enabled(False)
        except Exception as exc:
            logger.warning("Scene '%s': failed to disable during bulk load: %s",
                           getattr(self, 'name', ''), exc)
            return None
        return was_enabled

    def _end_bulk_scene_load(self, previous_enabled: Optional[bool]) -> None:
        if previous_enabled is None:
            return
        engine_scene = getattr(self, 'engine_scene', None)
        if engine_scene is None or not hasattr(engine_scene, 'set_enabled'):
            return
        try:
            engine_scene.set_enabled(previous_enabled)
        except Exception as exc:
            logger.warning("Scene '%s': failed to restore enabled state after load: %s",
                           getattr(self, 'name', ''), exc)

    def _read_data_unchecked(self):
        # 读取文件数据
        if os.path.isabs(self.route):
            data_path = self.route
        else:
            data_path = os.path.join(_active_project_path() or '', self.route)

        if os.path.exists(data_path):
            self.archive_snapshot = parse_archive(data_path)
            self.file_data.read(data_path, encoding='utf-8')
            if self._is_portable_scene_folder():
                self.name = self.file_data.get('scene', 'name', fallback=self.name)
            else:
                self.name = self.file_data.get('base', 'name', fallback=self.name)

            # 读取太阳设置
            if 'sun' in self.file_data:
                sun_direction_str = self.file_data['sun'].get('sun_direction', '1.0, 1.0, 1.0')
                sun_direction = [float(x.strip()) for x in sun_direction_str.split(',')]
                self.set_sun_direction(sun_direction, True)

                sun_enabled = self.file_data['sun'].getboolean('enabled', True)
                self.set_sun_enabled(sun_enabled, True)

            if 'grid' in self.file_data:
                grid_enabled = self.file_data['grid'].getboolean('enabled', True)
                self.set_floor_grid(grid_enabled, True)

            # Actor runtime/state is owned by the C++ native editor scene.
            # Keep the INI section available for compatibility, but never
            # instantiate Python Actor objects from it here. Otherwise stale
            # Python _actors can overwrite native deletes/imports on save.

            # 读取脚本数据
            if 'scripts' in self.file_data:
                self.script_path = self.file_data['scripts'].get('path', '')

            # 读取地形数据
            if 'terrain' in self.file_data:
                self.terrain_path = self.file_data['terrain'].get('path', '')
                self.terrain_type = self.file_data['terrain'].get('type', '')

            # 读取相机数据（延迟到 ensure_default_camera 之后应用）
            if 'vision_document' in self.file_data:
                section = self.file_data['vision_document']
                encoding = section.get('encoding', '')
                data = section.get('data', '')
                self.vision_document_asset_root = section.get('asset_root', '')
                if encoding == VISION_DOCUMENT_ENCODING and data:
                    try:
                        self.vision_document = _decode_vision_document(data)
                        self.vision_document_encoding = encoding
                    except Exception as exc:
                        logger.warning("Scene '%s': failed to decode embedded Vision document: %s",
                                       self.name, exc)
            if 'vision' in self.file_data:
                self.vision_storage = self.file_data['vision'].get('storage', '')
                self.vision_source_id = self.file_data['vision'].get('source_id', '')
                self.vision_source_path = self.file_data['vision'].get('source_path', '')
                self.vision_import_mode = self.file_data['vision'].get('import_mode', '')
            self.vision_bindings = self._read_indexed_section('vision_bindings')
            self.vision_unsupported_shapes = self._read_indexed_section('vision_unsupported_shapes')
            self._sync_external_vision_bindings_to_actors()

            self._pending_camera_data = {}
            if 'camera' in self.file_data:
                self._pending_camera_data = dict(self.file_data['camera'])

    def _apply_pending_camera_data(self):
        """加载新 camera 列表格式，并兼容旧的单 camera0.* 格式。"""
        data = getattr(self, '_pending_camera_data', {})
        if not data:
            return

        try:
            camera_count = int(data.get('count', 0))
        except (TypeError, ValueError):
            camera_count = 0
        if camera_count <= 0:
            indices = []
            for key in data:
                if key.startswith('camera') and '.' in key:
                    index_text = key[6:key.index('.')]
                    if index_text.isdigit():
                        indices.append(int(index_text))
            camera_count = max(indices, default=0) + 1

        while len(self._cameras) < camera_count:
            camera_index = len(self._cameras)
            camera = NativeCameraRecord(
                scene_route=self.route,
                name=f"{self.name}_Camera{camera_index}",
                deletable=camera_index != 0)
            self._cameras.append(camera)

        for i, cam in enumerate(self._cameras[:camera_count]):
            prefix = f'camera{i}'
            cam.camera_id = data.get(f'{prefix}.id', cam.camera_id)
            cam.name = data.get(f'{prefix}.name', cam.name)
            cam.deletable = data.get(
                f'{prefix}.deletable',
                'false' if i == 0 else 'true').lower() in ('1', 'true', 'yes', 'on')
            try:
                pos_str = data.get(f'{prefix}.position')
                fwd_str = data.get(f'{prefix}.forward')
                up_str = data.get(f'{prefix}.world_up')
                fov_str = data.get(f'{prefix}.fov')
                if pos_str and fwd_str and up_str and fov_str:
                    pos = [float(x.strip()) for x in pos_str.split(',')]
                    fwd = [float(x.strip()) for x in fwd_str.split(',')]
                    up = [float(x.strip()) for x in up_str.split(',')]
                    fov = float(fov_str.strip())
                    cam.set(pos, fwd, up, fov)
            except (TypeError, ValueError):
                logger.warning("Scene '%s': invalid pose for %s", self.name, prefix)

            width = int(data.get(f'{prefix}.width', cam.width))
            height = int(data.get(f'{prefix}.height', cam.height))
            cam.set_size(width, height)
            cam.set_output_mode(data.get(f'{prefix}.output_mode', 'final_color'))
            cam.set_ssao_enabled(
                data.get(f'{prefix}.ssao_enabled', 'true').lower() in ('1', 'true', 'yes', 'on'))
            cam.set_render_backend(data.get(f'{prefix}.render_backend', 'native'))
            try:
                cam.set_vision_render_mode(data.get(f'{prefix}.vision_render_mode'))
            except ValueError as exc:
                logger.warning("Scene '%s': invalid Vision render mode for %s: %s",
                               self.name, prefix, exc)
            cam.set_view_state(
                data.get(f'{prefix}.view_open', 'false').lower() in ('1', 'true', 'yes', 'on'),
                int(data.get(f'{prefix}.view_x', 120)),
                int(data.get(f'{prefix}.view_y', 120)),
                int(data.get(f'{prefix}.view_width', 960)),
                int(data.get(f'{prefix}.view_height', 540)),
                float(data.get(f'{prefix}.move_speed', 1.0)),
            )
            if i > 0:
                cam.set_surface(0)

        active_id = data.get('active_id')
        active = next(
            (camera for camera in self._cameras
             if camera.camera_id == active_id or camera.name == active_id),
            self._cameras[0] if self._cameras else None)
        if active is not None:
            self._main_camera = active
        self._pending_camera_data = {}

    def _preserve_native_actors_section(self, data_path: str) -> None:
        """Keep the C++ native scene as the owner of [actors]."""
        disk_data = configparser.ConfigParser()
        if os.path.exists(data_path):
            try:
                disk_data.read(data_path, encoding='utf-8')
            except Exception as exc:
                logger.warning("Scene '%s': failed to preserve native actors section: %s",
                               self.name, exc)

        if 'actors' in self.file_data:
            self.file_data.remove_section('actors')
        if 'actors' in disk_data:
            self.file_data.add_section('actors')
            for key, value in disk_data['actors'].items():
                self.file_data.set('actors', key, value)

    def save_data(self):
        if self._is_portable_scene_folder():
            # Portable scene saves are owned by the native scene store.  Python
            # only supplies the fields that are not already native scene state.
            from api.editor_api import CoronaEditorApi
            vision_document = getattr(self, 'vision_document', None)
            snapshot = {
                'name': self.name,
                'script_path': getattr(self, 'script_path', ''),
                'terrain': {
                    'path': getattr(self, 'terrain_path', ''),
                    'type': getattr(self, 'terrain_type', ''),
                },
                'vision': {
                    'storage': getattr(self, 'vision_storage', ''),
                    'source_id': getattr(self, 'vision_source_id', ''),
                    'import_mode': getattr(self, 'vision_import_mode', ''),
                },
                'vision_document': {
                    'version': VISION_DOCUMENT_VERSION,
                    'encoding': VISION_DOCUMENT_ENCODING,
                    'asset_root': getattr(self, 'vision_document_asset_root', ''),
                    'data': _encode_vision_document(vision_document) if vision_document is not None else '',
                },
            }
            scene_route = 'scene.ini' if os.path.isabs(self.route) else self.route
            result = CoronaEditorApi.main.scene_save(scene_route, snapshot)
            if isinstance(result, dict) and not result.get('ok', False):
                diagnostics = result.get('diagnostics', [])
                raise RuntimeError(
                    f"Portable scene save failed: {result.get('message', 'validation failed')}; "
                    f"diagnostics={diagnostics}")
            return result

        raise RuntimeError(
            'Legacy projects are read-only; use Save as Portable Scene before editing')

        # 保存文件数据
        if os.path.isabs(self.route):
            data_path = self.route
        else:
            data_path = os.path.join(_active_project_path() or '', self.route)

        # 确保必要的 section 存在。actors 由 C++ native scene 持久化，
        # Python save_data 只保留磁盘上的当前 actors section，不重新生成。
        metadata_section = 'scene' if self._is_portable_scene_folder() else 'base'
        for section in (metadata_section, 'sun', 'grid', 'scripts', 'terrain'):
            if section not in self.file_data:
                self.file_data[section] = {}

        if metadata_section == 'scene' and 'base' in self.file_data:
            self.file_data.remove_section('base')

        # 基础信息
        self.file_data[metadata_section]['name'] = self.name

        # 太阳设置
        env = self.get_environment()
        if env and hasattr(env, 'get_sun_direction'):
            sun_direction = env.get_sun_direction()
            self.file_data['sun']['sun_direction'] = f"{sun_direction[0]: .2f}, {sun_direction[1]: .2f}, {sun_direction[2]: .2f}"
        if env and hasattr(env, 'get_sun_intensity'):
            self.file_data['sun']['enabled'] = 'true' if env.get_sun_intensity() > 0.0 else 'false'
        if env and hasattr(env, 'get_floor_grid'):
            self.file_data['grid']['enabled'] = 'true' if env.get_floor_grid() else 'false'

        self._preserve_native_actors_section(data_path)

        # 脚本数据
        self.file_data['scripts']["path"] = getattr(self, 'script_path', '')

        # 地形数据
        self.file_data['terrain']["path"] = getattr(self, 'terrain_path', '')
        self.file_data['terrain']["type"] = getattr(self, 'terrain_type', '')

        vision_storage = (getattr(self, 'vision_storage', '') or '').strip()
        vision_source_id = (getattr(self, 'vision_source_id', '') or '').strip()
        vision_document = getattr(self, 'vision_document', None)
        if vision_document is not None:
            asset_root = getattr(self, 'vision_document_asset_root', '')
            if 'vision' not in self.file_data:
                self.file_data['vision'] = {}
            self.file_data['vision'].clear()
            self.file_data['vision']['storage'] = 'embedded'
            self.file_data['vision']['import_mode'] = 'external'
            if 'vision_document' not in self.file_data:
                self.file_data['vision_document'] = {}
            self.file_data['vision_document'].clear()
            self.file_data['vision_document']['version'] = VISION_DOCUMENT_VERSION
            self.file_data['vision_document']['encoding'] = VISION_DOCUMENT_ENCODING
            self.file_data['vision_document']['asset_root'] = asset_root
            self.file_data['vision_document']['data'] = _encode_vision_document(vision_document)
        elif vision_storage == 'project_sidecar' and vision_source_id:
            if 'vision' not in self.file_data:
                self.file_data['vision'] = {}
            self.file_data['vision'].clear()
            self.file_data['vision']['storage'] = 'project_sidecar'
            self.file_data['vision']['source_id'] = vision_source_id
            self.file_data['vision']['import_mode'] = 'external'
            if 'vision_document' in self.file_data:
                self.file_data.remove_section('vision_document')
        elif 'vision' in self.file_data:
            self.file_data.remove_section('vision')
            if 'vision_document' in self.file_data:
                self.file_data.remove_section('vision_document')
        if 'vision_bindings' in self.file_data:
            self.file_data.remove_section('vision_bindings')
        if 'vision_unsupported_shapes' in self.file_data:
            self.file_data.remove_section('vision_unsupported_shapes')

        # 相机数据
        self.file_data['camera'] = {}
        active_camera = self.get_active_camera()
        self.file_data['camera']['count'] = str(len(self._cameras))
        self.file_data['camera']['active_id'] = (
            active_camera.camera_id if active_camera is not None else '')
        for i, cam in enumerate(self._cameras):
            if cam is not None:
                try:
                    cam.refresh_view_state()
                    cam.refresh_size()
                    pos = cam.get_position()
                    fwd = cam.get_forward()
                    up = cam.get_world_up()
                    fov = cam.get_fov()
                    prefix = f'camera{i}'
                    self.file_data['camera'][f'{prefix}.id'] = cam.camera_id
                    self.file_data['camera'][f'{prefix}.name'] = cam.name
                    self.file_data['camera'][f'{prefix}.deletable'] = str(cam.deletable).lower()
                    self.file_data['camera'][f'{prefix}.position'] = f"{pos[0]:.4f}, {pos[1]:.4f}, {pos[2]:.4f}"
                    self.file_data['camera'][f'{prefix}.forward'] = f"{fwd[0]:.4f}, {fwd[1]:.4f}, {fwd[2]:.4f}"
                    self.file_data['camera'][f'{prefix}.world_up'] = f"{up[0]:.4f}, {up[1]:.4f}, {up[2]:.4f}"
                    self.file_data['camera'][f'{prefix}.fov'] = f"{fov:.2f}"
                    self.file_data['camera'][f'{prefix}.width'] = str(cam.width)
                    self.file_data['camera'][f'{prefix}.height'] = str(cam.height)
                    self.file_data['camera'][f'{prefix}.output_mode'] = cam.get_output_mode()
                    self.file_data['camera'][f'{prefix}.ssao_enabled'] = str(cam.get_ssao_enabled()).lower()
                    self.file_data['camera'][f'{prefix}.render_backend'] = cam.get_render_backend()
                    self.file_data['camera'][f'{prefix}.vision_render_mode'] = cam.get_vision_render_mode()
                    self.file_data['camera'][f'{prefix}.move_speed'] = str(cam.move_speed)
                    self.file_data['camera'][f'{prefix}.view_open'] = str(cam.view_open).lower()
                    self.file_data['camera'][f'{prefix}.view_x'] = str(cam.view_x)
                    self.file_data['camera'][f'{prefix}.view_y'] = str(cam.view_y)
                    self.file_data['camera'][f'{prefix}.view_width'] = str(cam.view_width)
                    self.file_data['camera'][f'{prefix}.view_height'] = str(cam.view_height)
                except Exception as exc:
                    logger.warning("Scene '%s': failed to save camera '%s': %s",
                                   self.name, getattr(cam, 'name', i), exc)

        with open(data_path, 'w', encoding='utf-8') as f:
            self.file_data.write(f)

    @auto_save
    def set_script(self, route):
        self.script_path = route
        return True

    @auto_save
    def set_terrain(self, route):
        self.terrain_path = route
        return True

    # Environment
    @auto_save
    def set_environment(self, environment: Environment, if_init=False) -> bool:
        self._environment = environment
        if hasattr(self.engine_scene, 'set_environment'):
            self.engine_scene.set_environment(
                environment.engine_obj if hasattr(environment, 'engine_obj') else environment)
            if if_init:
                return False
            return True
        return False

    def get_environment(self) -> Optional[Environment]:
        return self._environment

    # Actor 管理
    def _notify_scene_tree_changed(self):
        """通知 SceneBar 前端刷新场景树"""
        try:
            scene_name = getattr(self, 'route', '') or getattr(self, 'name', '')
            emit_compat_editor_event("scene-tree-changed", [scene_name])
        except Exception:
            pass

    @auto_save
    def add_actor(self, actor: Actor, rescene: bool = False) -> bool:
        if actor is None:
            return False
        adapter = get_scene_tools_adapter()
        creator = getattr(adapter, "create_actor", None) if adapter is not None else None
        if not callable(creator):
            logger.warning("Scene.add_actor ignored: SceneTools create_actor unavailable")
            return False

        actor_data = {
            "actor_name": getattr(actor, 'name', ''),
            "name": getattr(actor, 'name', ''),
            "actor_guid": getattr(actor, 'actor_guid', ''),
            "skip_if_exists": True,
        }
        for key, getter_name in (
            ("position", "get_position"),
            ("rotation", "get_rotation"),
            ("scale", "get_scale"),
        ):
            getter = getattr(actor, getter_name, None)
            if callable(getter):
                try:
                    actor_data[key] = list(getter())
                except Exception:
                    pass
        physics_getter = getattr(actor, 'get_physics_enabled', None)
        if callable(physics_getter):
            try:
                actor_data["physics_enabled"] = bool(physics_getter())
            except Exception:
                pass

        raw = creator(
            self.route,
            getattr(actor, 'route', ''),
            getattr(actor, 'actor_type', 'model'),
            actor_data,
        )
        try:
            result = json.loads(raw) if isinstance(raw, str) else raw
            native_actor = result.get("actor") if isinstance(result, dict) else None
            if isinstance(native_actor, dict):
                actor.name = native_actor.get("name", getattr(actor, 'name', ''))
                actor.actor_guid = native_actor.get("actor_guid", getattr(actor, 'actor_guid', ''))
        except Exception:
            logger.debug("Scene.add_actor native result could not be parsed: %r", raw)
        self._notify_scene_tree_changed()
        return True

    @auto_save
    def remove_actor(self, actor: Actor, rescene: bool = False) -> bool:
        logger.warning("Scene.remove_actor ignored: actor deletion is owned by C++ native scene")
        return False

    @auto_save
    def clear_actors(self, rescene: bool = False) -> bool:
        logger.warning("Scene.clear_actors ignored: actor deletion is owned by C++ native scene")
        return False

    # 相机管理
    @auto_save
    def add_camera_to_scene(self, camera: NativeCameraRecord) -> bool:
        if camera in self._cameras:
            return False
        self._cameras.append(camera)
        if self._main_camera is None:
            camera.deletable = False
            self._main_camera = camera
        return True

    @auto_save
    def remove_camera_from_scene(self, camera: NativeCameraRecord) -> bool:
        if camera not in self._cameras:
            return False
        self._cameras.remove(camera)
        if self._main_camera is camera:
            self._main_camera = self._cameras[0] if self._cameras else None
        return True

    @auto_save
    def clear_cameras(self) -> bool:
        for cam in self._cameras.copy():
            self.remove_camera_from_scene(cam)
        self._main_camera = None
        return True

    def get_cameras(self) -> List[NativeCameraRecord]:
        return self._cameras.copy()

    # 查询
    def get_actors(self) -> List[Actor]:
        return self._actors.copy()

    def get_actor(self, actor_name: str) -> Optional[Actor]:
        for actor in self._actors:
            if actor.name == actor_name:
                return actor
        return None

    # 太阳方向
    @auto_save
    def set_sun_direction(self, direction: List[float], if_init=False) -> bool:
        """设置太阳方向（主光源方向）- 委托给 Environment"""
        if self._environment:
            self._environment.set_sun_direction(direction)
        else:
            # 降级：如果没有 Environment，尝试直接调用引擎场景
            if hasattr(self.engine_scene, 'set_sun_direction'):
                self.engine_scene.set_sun_direction(direction)
        if if_init:
            return False
        return True

    @auto_save
    def set_sun_enabled(self, enabled: bool, if_init: bool = False) -> bool:
        if self._environment:
            if hasattr(self._environment, 'set_sun_intensity'):
                self._environment.set_sun_intensity(10.0 if enabled else 0.0)
            if hasattr(self._environment, 'set_sky_intensity'):
                self._environment.set_sky_intensity(20.0 if enabled else 0.0)
        if if_init:
            return False
        return True

    @auto_save
    def set_floor_grid(self, enabled: bool, if_init: bool = False) -> bool:
        """设置地面网格显示开关 - 委托给 Environment"""
        if self._environment and hasattr(self._environment, 'set_floor_grid'):
            self._environment.set_floor_grid(enabled)
        if if_init:
            return False
        return True

    def set_gravity(self, gravity: List[float]) -> bool:
        """设置重力向量 - 委托给 Environment"""
        if self._environment:
            self._environment.set_gravity(gravity)
        return True

    def get_gravity(self) -> List[float]:
        """获取重力向量"""
        if self._environment:
            return self._environment.get_gravity()
        return [0.0, -9.8, 0.0]

    def set_floor_y(self, y: float) -> bool:
        """设置地面高度 - 委托给 Environment"""
        if self._environment:
            self._environment.set_floor_y(y)
        return True

    def get_floor_y(self) -> float:
        """获取地面高度"""
        if self._environment:
            return self._environment.get_floor_y()
        return 0.0

    def set_floor_restitution(self, restitution: float) -> bool:
        """设置地面弹性系数 - 委托给 Environment"""
        if self._environment:
            self._environment.set_floor_restitution(restitution)
        return True

    def get_floor_restitution(self) -> float:
        """获取地面弹性系数"""
        if self._environment:
            return self._environment.get_floor_restitution()
        return 0.6

    def set_fixed_dt(self, dt: float) -> bool:
        """设置物理固定时间步长 - 委托给 Environment"""
        if self._environment:
            self._environment.set_fixed_dt(dt)
        return True

    def get_fixed_dt(self) -> float:
        """获取物理固定时间步长"""
        if self._environment:
            return self._environment.get_fixed_dt()
        return 1.0 / 60.0

    def get_aabb(self) -> list:
        """获取场景世界 AABB [min_x, min_y, min_z, max_x, max_y, max_z]"""
        return list(self.engine_scene.get_aabb())

    def set_enabled(self, enabled: bool) -> None:
        """启用或禁用场景（禁用后跳过渲染与物理模拟，但不销毁任何 C++ 对象）"""
        if hasattr(self.engine_scene, 'set_enabled'):
            self.engine_scene.set_enabled(enabled)

    def is_enabled(self) -> bool:
        """返回场景当前是否处于启用状态"""
        if hasattr(self.engine_scene, 'is_enabled'):
            return self.engine_scene.is_enabled()
        return True

    def set_simulation_enabled(self, enabled: bool) -> None:
        """启用或禁用该场景的物理模拟（不影响渲染）"""
        if hasattr(self.engine_scene, 'set_simulation_enabled'):
            self.engine_scene.set_simulation_enabled(enabled)

    def is_simulation_enabled(self) -> bool:
        """返回场景物理模拟是否启用"""
        if hasattr(self.engine_scene, 'is_simulation_enabled'):
            return self.engine_scene.is_simulation_enabled()
        return False

    def ensure_default_camera(self) -> bool:
        """确保场景至少有一个 Camera。"""
        created = False

        if not self._cameras:
            camera = NativeCameraRecord(
                scene_route=self.route,
                name=f"{self.name}_MainCamera",
                deletable=False)
            self._cameras.append(camera)
            self._main_camera = camera
            created = True

        if self._main_camera is None:
            self._main_camera = self._cameras[0]
            self._main_camera.deletable = False

        return created

    def get_active_camera(self) -> Optional[NativeCameraRecord]:
        self.ensure_default_camera()
        if not self._cameras:
            return None
        if self._main_camera is not None:
            return self._main_camera
        return self._cameras[0]

    def find_camera(self, camera_name: Optional[str]) -> Optional[NativeCameraRecord]:
        if not camera_name:
            return self.get_active_camera()

        for camera in self._cameras:
            if (getattr(camera, 'name', None) == camera_name or
                    getattr(camera, 'camera_id', None) == camera_name):
                return camera

        logger.warning("Scene.find_camera: camera '%s' not found in scene '%s'",
                       camera_name, self.name)
        return None

    # Camera 设置（兼容旧接口）
    @auto_save
    def set_camera(self, position, forward, up, fov: float,
                   camera_name: Optional[str] = None) -> bool:
        """设置摄像头参数"""
        self.ensure_default_camera()
        camera = self.find_camera(camera_name)

        if camera is not None:
            self._main_camera = camera

            logger.info("Scene.set_camera scene=%s camera=%s camera_type=%s",
                        self.name,
                        getattr(camera, 'name', camera_name),
                        type(camera).__name__)

            if hasattr(camera, 'set'):
                camera.set(position, forward, up, fov)
                return True

            if hasattr(camera, 'engine_obj') and hasattr(camera.engine_obj, 'set'):
                camera.engine_obj.set(position, forward, up, fov)
                return True

            logger.error("Scene.set_camera failed: camera object has no callable set() path")
            return False

        logger.error("Scene.set_camera failed: no camera available")
        return False

    def to_dict(self):
        """生成场景快照"""
        self.ensure_default_camera()

        sun_direction = [0.0, -1.0, 0.0]
        sun_enabled = True
        floor_grid_enabled = True
        env = self.get_environment()
        if env and hasattr(env, 'get_sun_direction'):
            sun_direction = env.get_sun_direction()
        if env and hasattr(env, 'get_sun_intensity'):
            sun_enabled = env.get_sun_intensity() > 0.0
        if env and hasattr(env, 'get_floor_grid'):
            floor_grid_enabled = env.get_floor_grid()

        active_camera = self.get_active_camera()
        camera_payloads = [cam.to_dict() for cam in self.get_cameras()]

        return {
            "id": self.route,
            "scene_id": self.route,
            "name": self.name,
            "active_camera_id": getattr(active_camera, 'camera_id', None),
            "active_camera_name": getattr(active_camera, 'name', None),
            "camera": active_camera.to_dict() if hasattr(active_camera, 'to_dict') else None,
            "cameras": camera_payloads,
            "sun": {
                "enabled": sun_enabled,
                "direction": sun_direction,
            },
            "grid": {
                "enabled": floor_grid_enabled,
            },
            "terrain": {
                "path": self.terrain_path,
                "type": self.terrain_type
            },
            "vision": {
                "storage": "embedded" if self.vision_document is not None else getattr(self, 'vision_storage', ''),
                "source_id": getattr(self, 'vision_source_id', ''),
                "import_mode": getattr(self, 'vision_import_mode', ''),
                "embedded": self.vision_document is not None,
                "bindings": list(getattr(self, 'vision_bindings', [])),
                "unsupported_shapes": list(getattr(self, 'vision_unsupported_shapes', [])),
            },
            "script": self.script_path,
            "actors": [actor.to_dict() for actor in self.get_actors()]
        }

    def find_actor(self, actor_name: str | None):
        """查找 Actor（支持模糊匹配）"""
        if not actor_name:
            return None

        actor = self.get_actor(actor_name)
        if actor:
            return actor

        def _normalize_actor_name(name: str) -> str:
            """标准化 Actor 名称（去除引号、扩展名等）"""
            value = name.strip().strip('"').strip("'")
            base = os.path.splitext(value.lower())[0]
            return base

        # 模糊匹配
        normalized = _normalize_actor_name(actor_name)
        for candidate in self.get_actors():
            if _normalize_actor_name(candidate.name) == normalized:
                return candidate
        return None

    def find_actor_by_route(self, route: str):
        """按文件路径查找 Actor"""
        for actor in self._actors:
            if actor.route == route:
                return actor
        return None

    def _build_actor_json(self, actors_section: configparser.SectionProxy, actor_name: str) -> Dict[str, Any]:
        """
        从INI配置构建actor的JSON数据

        Args:
            actors_section: actors节的配置数据
            actor_name: actor的名称

        Returns:
            包含actor完整信息的字典
        """
        actor_data = {
            "name": actors_section.get(f'{actor_name}.name', actor_name),
            "actor_type": actors_section.get(f'{actor_name}.actor_type', 'actor'),
            "route": actors_section.get(f'{actor_name}.route', ''),
            "actor_guid": actors_section.get(f'{actor_name}.actor_guid', ''),
            "geometry": {},
            "_suppress_network_broadcast": True
        }
        follow_camera_key = f'{actor_name}.follow_camera'
        if follow_camera_key in actors_section:
            actor_data["follow_camera"] = actors_section.getboolean(follow_camera_key)
        physics_enabled_key = f'{actor_name}.mechanics.physics_enabled'
        if physics_enabled_key in actors_section:
            actor_data["mechanics"] = {
                "physics_enabled": actors_section.getboolean(physics_enabled_key)
            }

        # 解析几何体属性
        pos_str = actors_section.get(f'{actor_name}.geometry.position', '0.0, 0.0, 0.0')
        rot_str = actors_section.get(f'{actor_name}.geometry.rotation', '0.0, 0.0, 0.0')
        scale_str = actors_section.get(f'{actor_name}.geometry.scale', '1.0, 1.0, 1.0')

        actor_data["geometry"]["position"] = [float(x.strip()) for x in pos_str.split(',')]
        actor_data["geometry"]["rotation"] = [float(x.strip()) for x in rot_str.split(',')]
        actor_data["geometry"]["scale"] = [float(x.strip()) for x in scale_str.split(',')]

        # 解析持久化的物理开关（与 save_data 写入的 {actor}.mechanics.physics_enabled 对称）。
        # configparser 值是字符串，"false" 直接 bool() 会变 True（非空串恒真）——必须显式
        # 比较转成真 bool，再放进 actor_data["mechanics"]，供 Actor._create_components_from_actor_data
        # 应用。字段缺失则不放该键（向后兼容：老场景行为不变，引擎默认物理开启）。
        phys_str = actors_section.get(f'{actor_name}.mechanics.physics_enabled', None)
        if phys_str is not None:
            actor_data["mechanics"] = {
                "physics_enabled": str(phys_str).strip().lower() == "true"
            }

        return actor_data

    def _read_indexed_section(self, section_name: str) -> List[Dict[str, Any]]:
        if section_name not in self.file_data:
            return []
        section = self.file_data[section_name]
        grouped: Dict[str, Dict[str, Any]] = {}
        for key, value in section.items():
            if '.' not in key:
                continue
            prefix, field = key.split('.', 1)
            grouped.setdefault(prefix, {})[field] = value
        def sort_key(item):
            prefix = item[0]
            if prefix.startswith('binding') and prefix[7:].isdigit():
                return (0, int(prefix[7:]))
            if prefix.startswith('shape') and prefix[5:].isdigit():
                return (0, int(prefix[5:]))
            return (1, prefix)
        return [fields for _, fields in sorted(grouped.items(), key=sort_key)]

    def _write_indexed_section(self, section_name: str, records: List[Dict[str, Any]]) -> None:
        if records:
            self.file_data[section_name] = {}
            for index, record in enumerate(records):
                prefix = f'binding{index}' if section_name == 'vision_bindings' else f'shape{index}'
                for key, value in record.items():
                    if section_name == 'vision_bindings' and key == 'source_path':
                        continue
                    if value is None:
                        continue
                    if isinstance(value, (list, tuple)):
                        serialized = ','.join(str(v) for v in value)
                    else:
                        serialized = str(value)
                    self.file_data[section_name][f'{prefix}.{key}'] = serialized
        elif section_name in self.file_data:
            self.file_data.remove_section(section_name)

    def _sync_external_vision_bindings_to_actors(self) -> None:
        bindings_by_actor = {
            record.get('actor_guid', ''): record
            for record in getattr(self, 'vision_bindings', [])
            if record.get('actor_guid', '')
        }
        external_live = (
            getattr(self, 'vision_document', None) is not None or
            getattr(self, 'vision_import_mode', '') == 'external_live'
        )
        for actor in getattr(self, '_actors', []):
            actor_guid = getattr(actor, 'actor_guid', '')
            binding = bindings_by_actor.get(actor_guid)
            if external_live and binding and hasattr(actor, 'set_external_vision_binding'):
                actor.set_external_vision_binding(binding)
            elif hasattr(actor, 'clear_external_vision_binding'):
                actor.clear_external_vision_binding()
