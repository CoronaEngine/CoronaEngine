import configparser
import json
import logging
import shutil
import time
import uuid
import weakref
from pathlib import Path
from typing import Any, Dict, Optional, List
import os

from ..components.geometry import Geometry
from ..components.mechanics import Mechanics
from ..components.acoustics import Acoustics
from ..components.optics import Optics
from ..corona_editor import CoronaEditor
from .. import network_sync_policy
from ...utils.proejct_utils import auto_save

CoronaEngine = CoronaEditor.CoronaEngine

_handle_to_actor = weakref.WeakValueDictionary()


def _active_project_path():
    try:
        from ...utils.settings import settings_manager
        if settings_manager.active_project_path:
            return settings_manager.active_project_path
    except Exception:
        pass
    return getattr(CoronaEngine, "active_project_path", None)


def _format_float(value) -> str:
    return format(float(value), ".17g")


def _format_float3(values) -> str:
    return ", ".join(_format_float(values[index]) for index in range(3))


class Actor:
    """
    OOP API 包装：基于 CoronaEngine.Actor。
    - 构造可选传入模型路径：自动创建 Geometry + 默认组件，组装成 Profile 加入 Actor
    - move/rotate/scale 改为作用于 active profile 的 Geometry（不再调用原生的旧接口）
    """

    def __init__(self, name='', route: Optional[str] = None, source_index: int = 0, actor_type: str = "actor",
                 parent_scene=None, actor_data=None):
        self.route = route
        if name:
            self.name = name
        else:
            self.name = Path(route).stem if route else ""
        self.actor_type = actor_type
        self.parent = parent_scene
        if source_index != 0:
            self.name = f"{self.name}_{source_index}"

        self.model_path = ""
        self.model_dependencies = []
        self.script_path = ""
        self.actor_guid = ""
        self._follow_camera = False
        self._suppress_network_broadcast = bool(
            actor_data and actor_data.get("_suppress_network_broadcast")
        )
        self.network_remote = self._suppress_network_broadcast
        self._collision_callback = None

        self.file_data = configparser.ConfigParser()

        if CoronaEngine is None:
            raise RuntimeError("CoronaEngine 未初始化")

        ActorCtor = getattr(CoronaEngine, 'Actor', None)
        if ActorCtor is None:
            raise RuntimeError("CoronaEngine 未提供 Actor 构造器")

        self.engine_obj = ActorCtor()

        # 解析数据路径和actor数据
        data_path = self._resolve_data_path()

        if actor_data:
            # 从场景中获取数据
            self._load_from_actor_data(actor_data, data_path)
        elif actor_type == "actor":
            # 从文件中读取数据
            self._load_from_config(data_path)
        else:
            self.final_model_path = data_path
            if not self.actor_guid:
                self.actor_guid = f"actor-{uuid.uuid4().hex}"
            self._geometry = self._build_geometry(self.final_model_path)
            self._create_and_add_profile()
            if self.actor_type == "ui_image":
                self._apply_ui_image_defaults()

        if not self.actor_guid:
            self.actor_guid = f"actor-{uuid.uuid4().hex}"

        self.handle = self.engine_obj.get_handle()
        self._sync_actor_guid_to_engine()
        self._sync_follow_camera_to_engine()
        _handle_to_actor[self.handle] = self

        self._enable_collision_callback = True  # 控制是否启用碰撞回调（Python 层回调开关）
        self._collision_type = 'box'           # 碰撞检测类型：'none'（关闭）/ 'box'（包围盒）/ 'mesh'（网格）
        self._camera_locked_script = None  # CameraLockedObject 脚本引用
        if self.parent:
            if self.network_remote:
                self._disable_local_physics_for_remote_actor()
            self._setup_collision_callback()
            self._setup_on_move_callback()
            if (self.actor_type != "actor" and
                    not self._suppress_network_broadcast and
                    hasattr(self, '_geometry')):
                self._broadcast_actor_created()

    def _resolve_data_path(self) -> Optional[str]:
        """解析数据文件的完整路径"""
        if not self.route:
            return None

        if os.path.isabs(self.route):
            return self.route
        else:
            return os.path.join(_active_project_path() or '', self.route)

    def _load_from_config(self, data_path: str):
        """从配置文件加载actor数据"""
        self.file_data.read(data_path, encoding='utf-8')

        # 读取模型路径
        saved_name = self.file_data['base'].get('name', '')
        if saved_name:
            self.name = saved_name
        self.model_path = self.file_data['base']['path']
        self.actor_guid = self.file_data['base'].get('actor_guid', '')
        self._follow_camera = self.file_data['base'].getboolean('follow_camera', fallback=False)
        if not self.actor_guid:
            self.actor_guid = f"actor-{uuid.uuid4().hex}"
        if self.model_path:
            self.final_model_path = os.path.join(_active_project_path() or '', self.model_path)
            if self.parent:
                self._create_components_from_config()

        self.script_path = self.file_data['scripts']["path"]

    def _load_from_actor_data(self, actor_data: dict, data_path: str):
        """从actor_data字典加载actor数据"""
        saved_name = actor_data.get('name', '')
        if saved_name:
            self.name = str(saved_name)
        file_follow_camera = False
        if self.actor_type == "actor":
            self.file_data.read(data_path, encoding='utf-8')
            self.model_path = self.file_data['base']['path']
            self.actor_guid = actor_data.get(
                'actor_guid',
                self.file_data['base'].get('actor_guid', '')
            )
            file_follow_camera = self.file_data['base'].getboolean('follow_camera', fallback=False)
            self.script_path = self.file_data['scripts']["path"]
            self.final_model_path = os.path.join(_active_project_path() or '',
                                                 self.model_path) if self.model_path else ""
        else:
            self.final_model_path = data_path
            self.actor_guid = actor_data.get('actor_guid', '')

        self._follow_camera = self._coerce_bool(
            actor_data.get('follow_camera',
                           actor_data.get('render_space', 'ui' if file_follow_camera else 'scene') == 'ui')
        )

        if not self.actor_guid:
            self.actor_guid = f"actor-{uuid.uuid4().hex}"

        if self.final_model_path:
            self._create_components_from_actor_data(actor_data)

    def _create_components_from_config(self):
        """从配置文件创建几何体和组件"""
        if not os.path.exists(self.final_model_path):
            raise FileNotFoundError(f"模型文件不存在: {self.final_model_path}")

        # 创建几何体
        self._geometry = Geometry(self.final_model_path)

        # 从配置文件读取变换参数
        position_str = self.file_data['geometry'].get('position', '0.0, 0.0, 0.0')
        position = [float(x.strip()) for x in position_str.split(',')]
        self.set_position(position, True)

        rotation_str = self.file_data['geometry'].get('rotation', '0.0, 0.0, 0.0')
        rotation = [float(x.strip()) for x in rotation_str.split(',')]
        self.set_rotation(rotation, True)

        scale_str = self.file_data['geometry'].get('scale', '1.0, 1.0, 1.0')
        scale = [float(x.strip()) for x in scale_str.split(',')]
        self.set_scale(scale, True)

        self._create_and_add_profile()
        self._apply_mechanics_data({
            "physics_enabled": self.file_data.getboolean(
                'mechanics', 'physics_enabled', fallback=True)
        })

    def _create_components_from_actor_data(self, actor_data: dict):
        """从actor_data字典创建几何体和组件"""
        if not os.path.exists(self.final_model_path):
            raise FileNotFoundError(f"模型文件不存在: {self.final_model_path}")

        # 创建几何体
        self._geometry = self._build_geometry(self.final_model_path)

        # 从actor_data读取变换参数
        self.set_position(actor_data["geometry"]["position"], True)
        self.set_rotation(actor_data["geometry"]["rotation"], True)
        self.set_scale(actor_data["geometry"]["scale"], True)

        self._create_and_add_profile()
        self._apply_mechanics_data(actor_data.get("mechanics", {}))

    def _build_geometry(self, path: str):
        """根据 actor_type 选择几何构造方式：
        ui_image → 程序化贴图 quad（Geometry.from_image）；其余 → 普通模型 Geometry。"""
        if self.actor_type == "ui_image" and hasattr(Geometry, "from_image"):
            return Geometry.from_image(path)
        return Geometry(path)

    def _apply_ui_image_defaults(self):
        """UI 图片平面的默认表现：作为 UI（跟随相机、屏幕正中、大小适中），
        不受光照、不参与物理。位置/大小用户后续可手动调整。"""
        # 默认作为 UI：follow_camera=True（其 setter 也会自动关闭物理）。
        try:
            self.set_follow_camera(True)
        except Exception:
            pass
        # UI 不受场景光照影响。
        if hasattr(self, "_optics") and self._optics is not None:
            try:
                self._optics.set_lighting_enabled(False)
            except Exception:
                pass
        # 默认放在相机正前方居中、大小适中（正交视口高度为 2.0，平面本地高度 1.0，
        # scale 0.8 → 约占屏幕高度的 40%，不铺满也不太小）。
        try:
            self.set_position([0.0, 0.0, 0.0], True)
            self.set_scale([0.8, 0.8, 0.8], True)
        except Exception:
            pass

        # 恢复持久化的物理开关（与 to_dict() 序列化的 mechanics.physics_enabled 对称）。
        # 向后兼容：字段缺失时保持引擎默认（开启），只有显式存了的 actor 才生效——
        # 否则 AI 摆放/已禁用物理的物体冷重载后又默认开启，进 play 模式被求解器推得
        # 东歪西斜（甚至互相穿插导致求解器死循环）。零回归：老场景无此字段，行为不变。
        mech_data = actor_data.get("mechanics")
        if isinstance(mech_data, dict) and "physics_enabled" in mech_data:
            if hasattr(self, "_mechanics") and self._mechanics is not None:
                try:
                    self._mechanics.set_physics_enabled(bool(mech_data["physics_enabled"]))
                except Exception:
                    pass

    def _create_profile_for_geometry(self, geometry):
        """Create an engine profile for a geometry and return its Python wrappers."""
        ActorProfile = getattr(CoronaEngine, 'ActorProfile', None)
        if ActorProfile is None:
            raise RuntimeError("CoronaEngine 未提供 ActorProfile 类型")

        # 创建各个组件
        optics = Optics(geometry)
        mechanics = Mechanics(geometry)
        acoustics = Acoustics(geometry)

        # 创建并配置profile
        prof = ActorProfile()
        prof.geometry = geometry.engine_obj
        prof.optics = optics.engine_obj
        prof.mechanics = mechanics.engine_obj
        prof.acoustics = acoustics.engine_obj

        # 添加到actor并激活
        stored = self.engine_obj.add_profile(prof)
        if stored is None:
            raise RuntimeError("无法向 Actor 添加默认 Profile（几何/组件不一致）")
        self.engine_obj.set_active_profile(stored)
        return stored, optics, mechanics, acoustics

    def _create_and_add_profile(self):
        """创建组件、配置集合并添加到actor"""
        stored, optics, mechanics, acoustics = self._create_profile_for_geometry(self._geometry)
        self._profile = stored
        self._optics = optics
        self._mechanics = mechanics
        self._acoustics = acoustics

    def _resolve_model_path(self, route: str) -> str:
        if not route:
            return ""
        if os.path.isabs(route):
            return route
        return os.path.join(_active_project_path() or '', route)

    @staticmethod
    def _safe_call(obj, method_name, default=None):
        if obj is None or not hasattr(obj, method_name):
            return default
        try:
            return getattr(obj, method_name)()
        except Exception as exc:
            logging.warning("Failed to read %s from %s: %s", method_name, obj, exc)
            return default

    @staticmethod
    def _safe_set(obj, method_name, value):
        if obj is None or value is None or not hasattr(obj, method_name):
            return
        try:
            getattr(obj, method_name)(value)
        except Exception as exc:
            logging.warning("Failed to restore %s on %s: %s", method_name, obj, exc)

    @staticmethod
    def _safe_set_many(obj, method_name, values):
        if obj is None or values is None or not hasattr(obj, method_name):
            return
        try:
            getattr(obj, method_name)(*values)
        except Exception as exc:
            logging.warning("Failed to restore %s on %s: %s", method_name, obj, exc)

    def _capture_model_state(self) -> Dict[str, Any]:
        return {
            'position': self._safe_call(getattr(self, '_geometry', None), 'get_position'),
            'rotation': self._safe_call(getattr(self, '_geometry', None), 'get_rotation'),
            'scale': self._safe_call(getattr(self, '_geometry', None), 'get_scale'),
            'optics': (self._safe_call(getattr(self, '_optics', None), 'to_dict', {}) or {}),
            'mechanics': (self._safe_call(getattr(self, '_mechanics', None), 'to_dict', {}) or {}),
            'collision_type': getattr(self, '_collision_type', 'box'),
        }

    def _restore_model_state(self, state: Dict[str, Any]):
        if state.get('position') is not None:
            self._geometry.set_position(state['position'])
        if state.get('rotation') is not None:
            self._geometry.set_rotation(state['rotation'])
        if state.get('scale') is not None:
            self._geometry.set_scale(state['scale'])

        optics_state = state.get('optics') or {}
        optics_setters = {
            'metallic': 'set_metallic',
            'roughness': 'set_roughness',
            'subsurface': 'set_subsurface',
            'specular': 'set_specular',
            'specular_tint': 'set_specular_tint',
            'anisotropic': 'set_anisotropic',
            'sheen': 'set_sheen',
            'sheen_tint': 'set_sheen_tint',
            'clearcoat': 'set_clearcoat',
            'clearcoat_gloss': 'set_clearcoat_gloss',
            'visible': 'set_visible',
            'ambient': 'set_ambient',
            'diffuse': 'set_diffuse',
            'specular_color': 'set_specular_color',
            'shininess': 'set_shininess',
        }
        for key, setter in optics_setters.items():
            self._safe_set(getattr(self, '_optics', None), setter, optics_state.get(key))

        mechanics_state = state.get('mechanics') or {}
        self._safe_set(getattr(self, '_mechanics', None), 'set_mass', mechanics_state.get('mass'))
        self._safe_set(getattr(self, '_mechanics', None), 'set_restitution', mechanics_state.get('restitution'))
        self._safe_set(getattr(self, '_mechanics', None), 'set_damping', mechanics_state.get('damping'))
        self._safe_set(getattr(self, '_mechanics', None), 'set_physics_enabled',
                       mechanics_state.get('physics_enabled'))
        self._safe_set_many(getattr(self, '_mechanics', None), 'set_linear_lock',
                            mechanics_state.get('linear_lock'))
        self._safe_set_many(getattr(self, '_mechanics', None), 'set_angular_lock',
                            mechanics_state.get('angular_lock'))

        self.set_collision_enabled(state.get('collision_type', 'box'))

    def _apply_mechanics_data(self, mechanics_data: dict):
        if not isinstance(mechanics_data, dict):
            return
        if not hasattr(self, '_mechanics') or self._mechanics is None:
            return
        if "physics_enabled" in mechanics_data:
            self.set_physics_enabled(self._coerce_bool(mechanics_data.get("physics_enabled")))

    @staticmethod
    def _coerce_bool(value) -> bool:
        if isinstance(value, str):
            return value.strip().lower() in ("1", "true", "yes", "on", "ui")
        return bool(value)

    @staticmethod
    def _coerce_float3(value):
        if isinstance(value, str):
            value = [part.strip() for part in value.split(',')]
        if not isinstance(value, (list, tuple)) or len(value) < 3:
            return None
        try:
            return [float(value[0]), float(value[1]), float(value[2])]
        except (TypeError, ValueError):
            return None

    @staticmethod
    def _obj_native_local_correction(model_path: str):
        if not model_path:
            return None
        path = Path(model_path)
        if not path.is_absolute():
            project_root = _active_project_path()
            if project_root:
                path = Path(project_root) / path
        if path.suffix.lower() != ".obj" or not path.is_file():
            return None

        vertices = []
        try:
            with path.open("r", encoding="utf-8", errors="ignore") as file:
                for line in file:
                    stripped = line.strip()
                    if not stripped.startswith("v "):
                        continue
                    parts = stripped.split()
                    if len(parts) < 4:
                        continue
                    vertices.append([float(parts[1]), float(parts[2]), float(parts[3])])
        except (OSError, ValueError) as exc:
            logging.warning("Failed to parse native Vision correction for %s: %s", path, exc)
            return None

        if not vertices:
            return None
        mins = [min(vertex[i] for vertex in vertices) for i in range(3)]
        maxs = [max(vertex[i] for vertex in vertices) for i in range(3)]
        center = [(mins[i] + maxs[i]) * 0.5 for i in range(3)]
        max_axis = max(maxs[i] - mins[i] for i in range(3))
        if max_axis <= 1e-8:
            max_axis = 1.0
        return [center[0], center[1], -center[2]], max_axis

    def _sync_follow_camera_to_engine(self):
        if hasattr(self.engine_obj, 'set_follow_camera'):
            try:
                self.engine_obj.set_follow_camera(self._follow_camera)
            except Exception as exc:
                logging.warning("Failed to sync follow_camera for actor %s: %s",
                                self.name or self.route, exc)

    def _sync_actor_guid_to_engine(self):
        if hasattr(self.engine_obj, 'set_actor_guid'):
            try:
                self.engine_obj.set_actor_guid(self.actor_guid)
            except Exception as exc:
                logging.warning("Failed to sync actor_guid for actor %s: %s",
                                self.name or self.route, exc)

    def set_external_vision_binding(self, binding: Dict[str, Any]):
        self._external_vision_binding = dict(binding or {})
        self._apply_external_vision_native_correction()
        setter = getattr(self.engine_obj, 'set_external_vision_binding', None)
        if not callable(setter):
            return
        try:
            shape_index = self._external_vision_binding.get("shape_index", -1)
            try:
                shape_index = int(shape_index)
            except (TypeError, ValueError):
                shape_index = -1
            setter(
                self._external_vision_binding.get("source_path", "") or "",
                self._external_vision_binding.get("shape_guid", "") or "",
                shape_index,
                self._external_vision_binding.get("json_path", "") or "",
                self._external_vision_binding.get("shape_type", "") or "",
                self._external_vision_binding.get("shape_identity_key", "") or "",
                self._external_vision_binding.get("model_path", "") or "",
            )
        except Exception as exc:
            logging.warning("Failed to sync external Vision binding for actor %s: %s",
                            self.name or self.route, exc)

    def _apply_external_vision_native_correction(self):
        if not hasattr(self, '_geometry') or self._geometry is None:
            return
        setter = getattr(self._geometry, 'set_native_local_correction', None)
        if not callable(setter):
            return

        binding = getattr(self, '_external_vision_binding', {}) or {}
        offset = self._coerce_float3(binding.get("native_local_correction_offset"))
        scale = binding.get("native_local_correction_scale", None)
        if offset is None and str(binding.get("shape_type", "")).lower() == "model":
            computed = self._obj_native_local_correction(
                binding.get("model_path", "") or getattr(self, "final_model_path", ""))
            if computed:
                offset, scale = computed
        if offset is None:
            offset = [0.0, 0.0, 0.0]
        try:
            scale = float(1.0 if scale is None else scale)
        except (TypeError, ValueError):
            scale = 1.0

        try:
            setter(offset, scale)
        except Exception as exc:
            logging.warning("Failed to apply native Vision correction for actor %s: %s",
                            self.name or self.route, exc)

    def clear_external_vision_binding(self):
        self._external_vision_binding = {}
        if hasattr(self, '_geometry') and self._geometry is not None:
            setter = getattr(self._geometry, 'set_native_local_correction', None)
            if callable(setter):
                try:
                    setter([0.0, 0.0, 0.0], 1.0)
                except Exception as exc:
                    logging.warning("Failed to clear native Vision correction for actor %s: %s",
                                    self.name or self.route, exc)
        clearer = getattr(self.engine_obj, 'clear_external_vision_binding', None)
        if not callable(clearer):
            return
        try:
            clearer()
        except Exception as exc:
            logging.warning("Failed to clear external Vision binding for actor %s: %s",
                            self.name or self.route, exc)

    def _broadcast_actor_created(self):
        """通过 NetworkSystem 广播 Actor 创建事件到已连接的 peer。"""
        try:
            network_sync_policy.publish_actor_created(
                self,
                prepare=lambda actor: actor._ensure_network_model_path_in_project(),
                emit=lambda actor_data: CoronaEditor.js_call_func(
                    "actor-sync-broadcast",
                    [actor_data],
                ),
            )
        except Exception as exc:
            logging.warning("Actor network create broadcast failed for %s: %s",
                            self.name or self.route, exc)

    def _broadcast_actor_transform_updated(self):
        """Broadcast a demo-grade transform delta for an already-synced Actor."""
        try:
            if self.network_remote or self._suppress_network_broadcast:
                return
            if not network_sync_policy.actor_is_syncable(self):
                return
            payload = {
                "actor_guid": self.actor_guid,
                "scene": self.parent.route if self.parent else "",
                "name": self.name,
                "actor_type": self.actor_type,
                "geometry": {
                    "position": list(self.get_position()),
                    "rotation": list(self.get_rotation()),
                    "scale": list(self.get_scale()),
                },
                "source_user_id": "",
                "correlation_id": "",
            }
            CoronaEditor.js_call_func("actor-transform-sync-broadcast", [payload])
        except Exception as exc:
            logging.warning("Actor network transform broadcast failed for %s: %s",
                            self.name or self.route, exc)

    def _disable_local_physics_for_remote_actor(self):
        if not hasattr(self, '_mechanics') or self._mechanics is None:
            return
        try:
            self._mechanics.set_physics_enabled(False)
        except Exception as exc:
            logging.warning("Failed to disable local physics for remote actor %s: %s",
                            self.name or self.route, exc)

    def _ensure_network_model_path_in_project(self):
        """Ensure network-created model paths are project-relative and transferable."""
        if self.actor_type == "actor" or not self.route:
            return

        project_root_raw = _active_project_path()
        if not project_root_raw:
            return

        project_root = Path(project_root_raw).resolve()
        route_path = Path(self.route)
        source_path = route_path if route_path.is_absolute() else (project_root / route_path)
        source_path = source_path.resolve()
        source_path = self._prefer_runtime_model_path(source_path)

        try:
            source_path.relative_to(project_root)
            rel_path = source_path.relative_to(project_root).as_posix()
            local_model_subdir = self._local_model_library_resource_subdir(rel_path)
            if local_model_subdir:
                if not source_path.is_file():
                    logging.warning("Actor local model library path is missing: %s",
                                    source_path)
                    return
                copied_paths = self._copy_model_asset_bundle_to_project(
                    source_path,
                    project_root,
                    target_subdir=local_model_subdir,
                )
                if not copied_paths:
                    return
                self.route = copied_paths[0]
                self.model_path = copied_paths[0]
                self.final_model_path = str(project_root / copied_paths[0])
                self.model_dependencies = copied_paths[1:]
                return
            stable_model_subdir = self._project_models_resource_subdir(rel_path)
            if stable_model_subdir:
                if not source_path.is_file():
                    logging.warning("Actor project models path is missing: %s",
                                    source_path)
                    return
                copied_paths = self._copy_model_asset_bundle_to_project(
                    source_path,
                    project_root,
                    target_subdir=stable_model_subdir,
                )
                if not copied_paths:
                    return
                self.route = copied_paths[0]
                self.model_path = copied_paths[0]
                self.final_model_path = str(project_root / copied_paths[0])
                self.model_dependencies = copied_paths[1:]
                return
            self.route = rel_path
            self.model_path = rel_path
            self.final_model_path = str(source_path)
            if source_path.is_file():
                self.model_dependencies = self._project_relative_dependency_paths(
                    source_path, project_root)
            return
        except ValueError:
            pass

        if not source_path.is_file():
            logging.warning("Actor network model path is outside project but missing: %s",
                            source_path)
            return

        copied_paths = self._copy_model_asset_bundle_to_project(source_path, project_root)
        if not copied_paths:
            return

        self.route = copied_paths[0]
        self.model_path = copied_paths[0]
        self.final_model_path = str(project_root / copied_paths[0])
        self.model_dependencies = copied_paths[1:]

    @staticmethod
    def _prefer_runtime_model_path(source_path: Path) -> Path:
        if not source_path.is_file() or source_path.parent.name == "runtime":
            return source_path
        runtime_candidate = source_path.parent / "runtime" / source_path.name
        if runtime_candidate.is_file():
            return runtime_candidate.resolve()
        if source_path.parent.name == "original":
            sibling_candidate = source_path.parent.parent / "runtime" / source_path.name
            if sibling_candidate.is_file():
                return sibling_candidate.resolve()
        return source_path

    @staticmethod
    def _local_model_library_resource_subdir(rel_path: str) -> Optional[str]:
        normalized = rel_path.replace("\\", "/")
        prefix = "assets/local_model_library/"
        if not normalized.startswith(prefix):
            return None
        local_rel = normalized[len("assets/"):]
        return Path(local_rel).parent.as_posix()

    @staticmethod
    def _project_models_resource_subdir(rel_path: str) -> Optional[str]:
        normalized = rel_path.replace("\\", "/")
        prefix = "models/"
        if not normalized.startswith(prefix):
            return None
        return Path(normalized).parent.as_posix()

    def _copy_model_asset_bundle_to_project(self, source_path: Path,
                                            project_root: Path,
                                            target_subdir: Optional[str] = None) -> List[str]:
        resource_dir = project_root / "Resource"
        resource_dir.mkdir(parents=True, exist_ok=True)
        target_dir = resource_dir
        if target_subdir:
            target_dir = resource_dir / target_subdir

        copied = []

        def copy_relative(src: Path, relative_to_source_dir: Path = None):
            rel_under_source = (src.name if relative_to_source_dir is None
                                else src.relative_to(relative_to_source_dir).as_posix())
            dst = target_dir / rel_under_source
            dst.parent.mkdir(parents=True, exist_ok=True)
            if src.resolve() != dst.resolve():
                shutil.copy2(src, dst)
            rel_project = dst.relative_to(project_root).as_posix()
            if rel_project not in copied:
                copied.append(rel_project)
            return dst

        copied_model_path = copy_relative(source_path)

        suffix = source_path.suffix.lower()
        if suffix == ".obj":
            source_dir = source_path.parent
            for mtl_path in self._read_obj_material_libraries(source_path):
                mtl_source = (source_dir / mtl_path).resolve()
                if not mtl_source.is_file():
                    logging.warning("OBJ material library missing: %s", mtl_source)
                    continue
                copy_relative(mtl_source, source_dir)
                for texture_path in self._read_mtl_texture_paths(mtl_source):
                    texture_source = (mtl_source.parent / texture_path).resolve()
                    if not texture_source.is_file():
                        logging.warning("MTL texture missing: %s", texture_source)
                        continue
                    copy_relative(texture_source, source_dir)
        elif suffix == ".gltf":
            for dep_path in self._collect_gltf_dependencies(source_path):
                copy_relative(dep_path, source_path.parent)
        elif suffix in {".fbx", ".dae", ".usd"}:
            for dep_path in self._collect_common_material_dependencies(source_path):
                copy_relative(dep_path, source_path.parent)

        if copied and copied[0] != copied_model_path.relative_to(project_root).as_posix():
            copied.insert(0, copied_model_path.relative_to(project_root).as_posix())
        return copied

    def _project_relative_dependency_paths(self, source_path: Path,
                                           project_root: Path) -> List[str]:
        dependencies = []
        for dep_path in self._collect_model_dependency_sources(source_path):
            try:
                rel_path = dep_path.resolve().relative_to(project_root).as_posix()
            except ValueError:
                logging.warning("Skipping dependency outside project: %s", dep_path)
                continue
            if rel_path not in dependencies:
                dependencies.append(rel_path)
        return dependencies

    def _collect_model_dependency_sources(self, source_path: Path) -> List[Path]:
        suffix = source_path.suffix.lower()
        dependencies = []
        if suffix == ".obj":
            source_dir = source_path.parent
            for mtl_path in self._read_obj_material_libraries(source_path):
                mtl_source = (source_dir / mtl_path).resolve()
                if not mtl_source.is_file():
                    logging.warning("OBJ material library missing: %s", mtl_source)
                    continue
                if mtl_source not in dependencies:
                    dependencies.append(mtl_source)
                for texture_path in self._read_mtl_texture_paths(mtl_source):
                    texture_source = (mtl_source.parent / texture_path).resolve()
                    if not texture_source.is_file():
                        logging.warning("MTL texture missing: %s", texture_source)
                        continue
                    if texture_source not in dependencies:
                        dependencies.append(texture_source)
        elif suffix == ".gltf":
            dependencies.extend(self._collect_gltf_dependencies(source_path))
        elif suffix in {".fbx", ".dae", ".usd"}:
            dependencies.extend(self._collect_common_material_dependencies(source_path))
        return dependencies

    @staticmethod
    def _read_obj_material_libraries(obj_path: Path) -> List[Path]:
        libraries = []
        try:
            with obj_path.open("r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    stripped = line.strip()
                    if not stripped or stripped.startswith("#"):
                        continue
                    parts = stripped.split(maxsplit=1)
                    if len(parts) == 2 and parts[0] == "mtllib":
                        libraries.append(Path(parts[1]))
        except Exception as exc:
            logging.warning("Failed to parse OBJ dependencies for %s: %s", obj_path, exc)
        return libraries

    @staticmethod
    def _read_mtl_texture_paths(mtl_path: Path) -> List[Path]:
        texture_keys = {
            "map_Ka", "map_Kd", "map_Ks", "map_Ke", "map_Ns",
            "map_d", "map_bump", "bump", "disp", "decal", "refl",
            "norm", "map_Pr", "map_Pm", "map_Ps", "map_Bump",
        }
        textures = []
        try:
            with mtl_path.open("r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    stripped = line.strip()
                    if not stripped or stripped.startswith("#"):
                        continue
                    parts = stripped.split()
                    if len(parts) < 2 or parts[0] not in texture_keys:
                        continue
                    textures.append(Path(parts[-1]))
        except Exception as exc:
            logging.warning("Failed to parse MTL dependencies for %s: %s", mtl_path, exc)
        return textures

    @staticmethod
    def _collect_gltf_dependencies(gltf_path: Path) -> List[Path]:
        dependencies = []
        try:
            data = json.loads(gltf_path.read_text(encoding="utf-8"))
            base_dir = gltf_path.parent
            uris = []
            for buffer in data.get("buffers", []) or []:
                uri = buffer.get("uri") if isinstance(buffer, dict) else None
                if uri:
                    uris.append(uri)
            for image in data.get("images", []) or []:
                uri = image.get("uri") if isinstance(image, dict) else None
                if uri:
                    uris.append(uri)
            for uri in uris:
                normalized = str(uri).strip()
                if not normalized or normalized.startswith("data:"):
                    continue
                dep_path = (base_dir / normalized).resolve()
                if dep_path.is_file() and dep_path not in dependencies:
                    dependencies.append(dep_path)
                elif not dep_path.is_file():
                    logging.warning("GLTF dependency missing: %s", dep_path)
        except Exception as exc:
            logging.warning("Failed to parse GLTF dependencies for %s: %s", gltf_path, exc)
        return dependencies

    @staticmethod
    def _collect_common_material_dependencies(model_path: Path) -> List[Path]:
        material_suffixes = {
            ".mtl", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".exr", ".hdr",
        }
        dependencies = []
        try:
            for candidate in sorted(model_path.parent.iterdir(),
                                    key=lambda path: path.name.lower()):
                if candidate == model_path or not candidate.is_file():
                    continue
                if candidate.suffix.lower() in material_suffixes:
                    dependencies.append(candidate.resolve())
        except Exception as exc:
            logging.warning("Failed to collect material dependencies for %s: %s",
                            model_path, exc)
        return dependencies

    def save_data(self):
        if self.parent:
            self.parent.save_data()
        else:
            self.file_data['base']['name'] = self.name
            self.file_data['base']['path'] = self.model_path
            self.file_data['base']['actor_guid'] = self.actor_guid
            self.file_data['base']['follow_camera'] = 'true' if self._follow_camera else 'false'
            if 'mechanics' not in self.file_data:
                self.file_data['mechanics'] = {}
            if hasattr(self, 'get_physics_enabled'):
                try:
                    self.file_data['mechanics']['physics_enabled'] = (
                        'true' if self.get_physics_enabled() else 'false'
                    )
                except Exception:
                    pass
            if self.model_path:
                position = self.get_position()
                rotation = self.get_rotation()
                scale = self.get_scale()
                self.file_data['geometry'][
                    f'position'] = _format_float3(position)
                self.file_data['geometry'][
                    f'rotation'] = _format_float3(rotation)
                self.file_data['geometry'][
                    f'scale'] = _format_float3(scale)

            self.file_data['scripts']['path'] = self.script_path

            with open(self._resolve_data_path(), 'w', encoding='utf-8') as f:
                self.file_data.write(f)

    @auto_save
    def set_route(self, route, source_index: int = 0):
        self.route = route
        self.name = Path(route).stem
        if source_index != 0:
            self.name = f"{self.name}_{source_index}"
        return True

    @auto_save
    def set_model(self, route):
        self.model_path = route
        self.final_model_path = self._resolve_model_path(route)

        if not self.final_model_path:
            return True

        old_profile = getattr(self, '_profile', None)
        if old_profile is None and hasattr(self.engine_obj, 'get_active_profile'):
            try:
                old_profile = self.engine_obj.get_active_profile()
            except Exception as exc:
                logging.warning("Failed to read active profile before model replace: %s", exc)

        state = self._capture_model_state()

        new_geometry = Geometry(self.final_model_path)
        stored, optics, mechanics, acoustics = self._create_profile_for_geometry(new_geometry)

        self._geometry = new_geometry
        self._profile = stored
        self._optics = optics
        self._mechanics = mechanics
        self._acoustics = acoustics

        self._restore_model_state(state)

        if old_profile is not None and old_profile is not stored and hasattr(self.engine_obj, 'remove_profile'):
            try:
                self.engine_obj.remove_profile(old_profile)
            except Exception as exc:
                logging.warning("Failed to remove old profile after model replace: %s", exc)

        # 重新设置回调（新的 mechanics 需要重新注册回调）
        self._setup_collision_callback()
        self._setup_on_move_callback()
        return True

    @auto_save
    def set_script(self, route):
        self.script_path = route
        return True

    # 兼容编辑器的变换操作：直接作用于几何体。
    # Contract:
    # - set_position/set_rotation/set_scale are absolute setters.
    # - translate/rotate_delta/scale_delta are relative operations.
    @auto_save
    def translate(self, delta: List[float]):
        if not hasattr(self, '_geometry'):
            return False
        pos = self._geometry.get_position()
        self._geometry.set_position([pos[0] + delta[0], pos[1] + delta[1], pos[2] + delta[2]])
        self._broadcast_actor_transform_updated()
        return True

    @auto_save
    def rotate_delta(self, delta: List[float]):
        if not hasattr(self, '_geometry'):
            return False
        rot = self._geometry.get_rotation()
        self._geometry.set_rotation([rot[0] + delta[0], rot[1] + delta[1], rot[2] + delta[2]])
        self._broadcast_actor_transform_updated()
        return True

    @auto_save
    def scale_delta(self, factor):
        if not hasattr(self, '_geometry'):
            return False
        if isinstance(factor, (int, float)):
            factor = [factor, factor, factor]
        current = self._geometry.get_scale()
        self._geometry.set_scale([
            current[0] * factor[0],
            current[1] * factor[1],
            current[2] * factor[2],
        ])
        self._broadcast_actor_transform_updated()
        return True

    def move(self, v: List[float]):
        return self.translate(v)

    def rotate(self, euler: List[float]):
        return self.rotate_delta(euler)

    def scale(self, v: List[float]):
        return self.set_scale(v)

    @auto_save
    def set_position(self, position: List[float], if_init=False):
        if not hasattr(self, '_geometry'):
            return False
        self._geometry.set_position(position)
        if if_init:
            return False
        self._broadcast_actor_transform_updated()
        return True

    def get_position(self) -> List[float]:
        if not hasattr(self, '_geometry'):
            raise RuntimeError("当前 Actor 没有 Geometry")
        return self._geometry.get_position()

    @auto_save
    def set_rotation(self, euler: List[float], if_init=False):
        if not hasattr(self, '_geometry'):
            return False
        self._geometry.set_rotation(euler)
        if if_init:
            return False
        self._broadcast_actor_transform_updated()
        return True

    def get_rotation(self) -> List[float]:
        if not hasattr(self, '_geometry'):
            raise RuntimeError("当前 Actor 没有 Geometry")
        return self._geometry.get_rotation()

    @auto_save
    def set_scale(self, scale: List[float], if_init=False):
        if not hasattr(self, '_geometry'):
            return False
        self._geometry.set_scale(scale)
        if if_init:
            return False
        self._broadcast_actor_transform_updated()
        return True

    def get_scale(self) -> List[float]:
        if not hasattr(self, '_geometry'):
            raise RuntimeError("当前 Actor 没有 Geometry")
        return self._geometry.get_scale()

    def set_visible(self, visible: bool):
        if not hasattr(self, '_optics'):
            raise RuntimeError("当前 Actor 没有 Optics")
        self._optics.set_visible(visible)

    def get_visible(self) -> bool:
        if not hasattr(self, '_optics'):
            return True
        return self._optics.get_visible()

    @auto_save
    def set_follow_camera(self, enabled: bool, if_init=False):
        self._follow_camera = self._coerce_bool(enabled)
        self._sync_follow_camera_to_engine()
        if if_init:
            return False
        if self._follow_camera and hasattr(self, '_mechanics') and self._mechanics is not None:
            self._mechanics.set_physics_enabled(False)
        return True

    def get_follow_camera(self) -> bool:
        if hasattr(self.engine_obj, 'get_follow_camera'):
            try:
                self._follow_camera = bool(self.engine_obj.get_follow_camera())
            except Exception as exc:
                logging.warning("Failed to read follow_camera for actor %s: %s",
                                self.name or self.route, exc)
        return self._follow_camera

    def set_mass(self, mass: float):
        if not hasattr(self, '_mechanics'):
            raise RuntimeError("当前 Actor 没有 Mechanics")
        self._mechanics.set_mass(mass)

    def get_mass(self) -> float:
        if not hasattr(self, '_mechanics'):
            raise RuntimeError("当前 Actor 没有 Mechanics")
        return self._mechanics.get_mass()

    def set_restitution(self, restitution: float):
        if not hasattr(self, '_mechanics'):
            raise RuntimeError("当前 Actor 没有 Mechanics")
        self._mechanics.set_restitution(restitution)

    def get_restitution(self) -> float:
        if not hasattr(self, '_mechanics'):
            raise RuntimeError("当前 Actor 没有 Mechanics")
        return self._mechanics.get_restitution()

    def set_damping(self, damping: float):
        if not hasattr(self, '_mechanics'):
            raise RuntimeError("当前 Actor 没有 Mechanics")
        self._mechanics.set_damping(damping)

    def get_damping(self) -> float:
        if not hasattr(self, '_mechanics'):
            raise RuntimeError("当前 Actor 没有 Mechanics")
        return self._mechanics.get_damping()

    def set_physics_enabled(self, enabled: bool):
        """启用或禁用该 Actor 的物理模拟（关闭后物体不参与力学模拟，但仍保留数据）"""
        if not hasattr(self, '_mechanics') or self._mechanics is None:
            raise RuntimeError("当前 Actor 没有 Mechanics")
        self._mechanics.set_physics_enabled(enabled)

    def get_physics_enabled(self) -> bool:
        """获取物理模拟开关状态"""
        if not hasattr(self, '_mechanics') or self._mechanics is None:
            raise RuntimeError("当前 Actor 没有 Mechanics")
        return self._mechanics.get_physics_enabled()

    def set_linear_lock(self, lock_x: bool, lock_y: bool, lock_z: bool):
        """设置线性运动轴锁定（锁定后该轴不参与平移运动）"""
        if not hasattr(self, '_mechanics') or self._mechanics is None:
            raise RuntimeError("当前 Actor 没有 Mechanics")
        self._mechanics.set_linear_lock(lock_x, lock_y, lock_z)

    def get_linear_lock(self):
        """获取线性运动轴锁定状态，返回 [lock_x, lock_y, lock_z]"""
        if not hasattr(self, '_mechanics') or self._mechanics is None:
            return [False, False, False]
        return list(self._mechanics.get_linear_lock())

    def set_angular_lock(self, lock_x: bool, lock_y: bool, lock_z: bool):
        """设置角度运动轴锁定（锁定后该轴不参与旋转运动）"""
        if not hasattr(self, '_mechanics') or self._mechanics is None:
            raise RuntimeError("当前 Actor 没有 Mechanics")
        self._mechanics.set_angular_lock(lock_x, lock_y, lock_z)

    def get_angular_lock(self):
        """获取角度运动轴锁定状态，返回 [lock_x, lock_y, lock_z]"""
        if not hasattr(self, '_mechanics') or self._mechanics is None:
            return [False, False, False]
        return list(self._mechanics.get_angular_lock())

    def set_collision_enabled(self, collision_type: str):
        """
        设置碰撞检测类型。

        Args:
            collision_type: 'none'（关闭碰撞检测）, 'box'（包围盒碰撞）, 'mesh'（网格碰撞）
        """
        self._collision_type = collision_type
        if hasattr(self, '_mechanics') and self._mechanics:
            self._mechanics.set_collision_enabled(collision_type != 'none')

    def get_collision_enabled(self) -> str:
        """获取碰撞检测类型：'none', 'box', 'mesh'"""
        return self._collision_type

    def _setup_collision_callback(self):
        """设置碰撞回调，使用类方法而非外部函数"""

        def collision_handler(other_handle, if_start, normal, point):
            if not self._enable_collision_callback:
                return

            other_actor = _handle_to_actor.get(other_handle)
            # 调用可重写的 on_collision 方法
            self.on_collision(other_actor, if_start, normal, point)

        if hasattr(self, '_mechanics') and self._mechanics:
            self._mechanics.set_collision_callback(collision_handler)

    def on_collision(self, other_actor, if_start, normal, point):
        """
        碰撞回调方法，子类可重写此方法以实现自定义行为。

        Args:
            other_actor: 碰撞的另一方 Actor 对象，可能为 None
            if_start: True 表示碰撞开始，False 表示碰撞结束
            normal: 碰撞法向量
            point: 碰撞点坐标
        """
        # 默认行为：记录碰撞信息
        if other_actor is not None:
            logging.info(f"{self.handle} Collision {'start' if if_start else 'end'} with {other_actor.name} at {point}")
        else:
            logging.info(f"{self.handle} Collision {'start' if if_start else 'end'} with unknown actor at {point}")


    def _setup_on_move_callback(self):
        """设置移动回调，使用类方法而非外部函数"""

        def on_move_handler():
            self.on_move()

        if hasattr(self, '_mechanics') and self._mechanics:
            self._mechanics.set_on_move_callback(on_move_handler)

    def on_move(self):
        """
        移动回调方法，子类可重写此方法以实现自定义行为。
        """
        CoronaEditor.js_call_func("transform-update",
                                  [self.parent.route if self.parent else "", self.name, self.get_position(),
                                   self.get_rotation(), self.get_scale(), self.actor_type])
        if self.actor_guid:
            CoronaEditor.js_call_func("actor-ownership-claim",
                                      [{"actor_guid": self.actor_guid}])
            self._broadcast_actor_transform_updated()
        self.save_data()


    def enable_collision_callback(self, enable: bool):
        """
        启用或禁用碰撞回调。

        Args:
            enable: True 启用回调，False 禁用回调
        """
        self._enable_collision_callback = enable

    # ========== 相机锁定相关 ==========

    def set_camera_locked_script(self, script):
        """设置 CameraLockedObject 脚本引用"""
        self._camera_locked_script = script

    def get_camera_lock_enabled(self) -> bool:
        if self._camera_locked_script is not None:
            return self._camera_locked_script.lock_to_camera
        return False

    def set_camera_lock_enabled(self, enabled: bool):
        if self._camera_locked_script is not None:
            self._camera_locked_script.toggle_lock(enabled)
        else:
            from ..scripts_system.entities.camera_locked_object import CameraLockedObject
            script = CameraLockedObject(f"Actor_{self.name}_CameraLock", self)
            script.initialize()
            self._camera_locked_script = script
            # 注册到 ScriptsManager
            from ..scripts_system.scripts_manager import ScriptsManager
            try:
                for mgr in ScriptsManager._instances:
                    mgr.actor_scripts[self.name] = script
                    break
            except Exception:
                pass
            script.toggle_lock(enabled)

    def get_camera_lock_offset(self) -> list:
        if self._camera_locked_script is not None:
            return self._camera_locked_script.position_offset
        return [0.0, 0.0, 2.0]

    def set_camera_lock_offset(self, offset: list):
        if self._camera_locked_script is not None:
            self._camera_locked_script.position_offset = list(offset)

    def get_camera_lock_rotation_offset(self) -> list:
        if self._camera_locked_script is not None:
            return self._camera_locked_script.rotation_offset
        return [0.0, 0.0, 0.0]

    def set_camera_lock_rotation_offset(self, offset: list):
        if self._camera_locked_script is not None:
            self._camera_locked_script.rotation_offset = list(offset)

    def to_dict(self) -> Dict[str, Any]:
        _, ext = os.path.splitext(self.route)
        follow_camera = self.get_follow_camera()

        result_dict = {
            "name": self.name,
            "actor_guid": self.actor_guid,
            "handle": int(self.handle),
            "path": self.route,
            "scene": self.parent.route if self.parent else "",
            "type": ext.lstrip("."),
            "model": self.model_path,
            "model_dependencies": list(self.model_dependencies),
            "actor_type": self.actor_type,
            "collision": self.get_collision_enabled(),
            "visible": self.get_visible(),
            "script": self.script_path,
            "follow_camera": follow_camera,
            "render_space": "ui" if follow_camera else "scene",
        }

        if hasattr(self, '_geometry'):
            result_dict["geometry"] = {
                "position": self.get_position(),
                "rotation": self.get_rotation(),
                "scale": self.get_scale(),
            }

        if hasattr(self, '_mechanics') and self._mechanics is not None:
            try:
                result_dict["mechanics"] = {
                    "mass": self.get_mass(),
                    "restitution": self.get_restitution(),
                    "damping": self.get_damping(),
                    "physics_enabled": self.get_physics_enabled(),
                    "linear_lock": self.get_linear_lock(),
                    "angular_lock": self.get_angular_lock(),
                }
            except Exception:
                pass

        # 相机锁定信息
        if self._camera_locked_script is not None:
            result_dict["camera_lock"] = self._camera_locked_script.to_dict()
        else:
            result_dict["camera_lock"] = {
                "lock_to_camera": False,
                "position_offset": [0.0, 0.0, 2.0],
                "rotation_offset": [0.0, 0.0, 0.0],
            }

        return result_dict

    def __repr__(self):
        return f"<Actor name={self.name} path={self.route}>"
