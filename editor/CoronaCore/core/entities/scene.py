import configparser
import logging
import os
from pathlib import Path
from typing import List, Optional, Dict, Any
from .actor import Actor
from .environment import Environment
from .camera import Camera

from ..corona_editor import CoronaEditor
from ...utils.proejct_utils import auto_save

CoronaEngine = CoronaEditor.CoronaEngine
logger = logging.getLogger(__name__)


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


class Scene:
    """
    场景包装类：仅管理对象引用，生命周期由外部管理。
    采用 OOP API：引擎 Scene 仅支持 Environment/Actor/Camera 管理。
    """

    def __init__(self, route):

        self.route = route
        self.name = Path(route).stem
        self.file_data = configparser.ConfigParser()

        # 创建引擎场景对象（OOP API）
        if CoronaEngine is None:
            raise RuntimeError("CoronaEngine 未初始化")
        SceneCtor = getattr(CoronaEngine, 'Scene', None)
        if SceneCtor is None:
            raise RuntimeError("CoronaEngine 未提供 Scene 构造器")
        self.engine_scene = SceneCtor()

        # 引用列表（Python 层）
        self._actors: List[Actor] = []
        self._cameras: List[Camera] = []
        self._main_camera: Optional[Camera] = None

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
        self.vision_source_path = ''
        self.vision_import_mode = ''
        self.vision_bindings: List[Dict[str, Any]] = []
        self.vision_unsupported_shapes: List[Dict[str, Any]] = []

        self.read_data()
        # 场景创建后补齐默认相机（幂等）
        self.ensure_default_camera()
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
            self.file_data.read(data_path, encoding='utf-8')

            # 读取太阳设置
            if 'sun' in self.file_data:
                sun_direction_str = self.file_data['sun'].get('sun_direction', '1.0, 1.0, 1.0')
                sun_direction = [float(x.strip()) for x in sun_direction_str.split(',')]
                self.set_sun_direction(sun_direction, True)

                sun_enabled = self.file_data['sun'].getboolean('enabled', True)
                self.set_floor_grid(sun_enabled, True)

            # 读取演员数据 - 构建JSON数据
            if 'actors' in self.file_data:
                actors_section = self.file_data['actors']
                # 收集所有演员的键
                actor_keys = set()
                for key in actors_section:
                    if '.' in key:
                        actor_name = key.split('.')[0]
                        actor_keys.add(actor_name)

                # 为每个演员构建JSON数据
                for actor_name in actor_keys:
                    try:
                        actor_data = self._build_actor_json(actors_section, actor_name)
                        source_index = sum(1 for a in self._actors if a.name == actor_data['name'])
                        # 创建 Actor 对象（传递JSON字符串）
                        actor = Actor(actor_name, actor_data['route'], source_index, actor_data['actor_type'],
                                      parent_scene=self, actor_data=actor_data)
                        if actor:
                            self._actors.append(actor)
                            # 同步注册到 C++ 引擎的 SceneDevice，否则 MechanicsSystem 等无法遍历到
                            if hasattr(self.engine_scene, 'add_actor') and hasattr(actor, 'engine_obj'):
                                self.engine_scene.add_actor(actor.engine_obj)
                    except Exception as e:
                        logger.warning("Scene '%s': 加载 actor '%s' 失败，已跳过：%s", self.name, actor_name, e)

            # 读取脚本数据
            if 'scripts' in self.file_data:
                self.script_path = self.file_data['scripts'].get('path', '')

            # 读取地形数据
            if 'terrain' in self.file_data:
                self.terrain_path = self.file_data['terrain'].get('path', '')
                self.terrain_type = self.file_data['terrain'].get('type', '')

            # 读取相机数据（延迟到 ensure_default_camera 之后应用）
            if 'vision' in self.file_data:
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
            camera = Camera(
                name=f"{self.name}_Camera{camera_index}",
                deletable=camera_index != 0)
            camera.set_surface(0)
            self._cameras.append(camera)
            self.engine_scene.add_camera(camera.engine_obj)

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
            self.engine_scene.set_active_camera(active.engine_obj)
        self._pending_camera_data = {}

    def save_data(self):
        # 保存文件数据
        if os.path.isabs(self.route):
            data_path = self.route
        else:
            data_path = os.path.join(_active_project_path() or '', self.route)

        # 确保必要的 section 存在
        for section in ('base', 'sun', 'actors', 'scripts', 'terrain'):
            if section not in self.file_data:
                self.file_data[section] = {}

        # 基础信息
        self.file_data['base']['name'] = self.name

        # 太阳设置
        env = self.get_environment()
        if env and hasattr(env, 'get_sun_direction'):
            sun_direction = env.get_sun_direction()
            self.file_data['sun']['sun_direction'] = f"{sun_direction[0]: .2f}, {sun_direction[1]: .2f}, {sun_direction[2]: .2f}"

        # 演员数据
        self.file_data['actors'] = {}
        used_keys = {}  # 记录已使用的 key，处理同名 actor
        for i, actor in enumerate(self._actors):
            # 使用 actor 的 name 属性或生成一个默认名称
            actor_name = getattr(actor, 'name', f'actor{i + 1}')

            # 确保序列化 key 唯一：同名时追加序号
            if actor_name in used_keys:
                used_keys[actor_name] += 1
                actor_key = f"{actor_name}_{used_keys[actor_name]}"
            else:
                used_keys[actor_name] = 0
                actor_key = actor_name

            self.file_data['actors'][f'{actor_key}.actor_type'] = getattr(actor, 'actor_type', 'actor')
            self.file_data['actors'][f'{actor_key}.name'] = actor_name
            self.file_data['actors'][f'{actor_key}.route'] = getattr(actor, 'route', '')
            actor_guid = getattr(actor, 'actor_guid', '')
            if actor_guid:
                self.file_data['actors'][f'{actor_key}.actor_guid'] = actor_guid
            if hasattr(actor, 'get_follow_camera'):
                self.file_data['actors'][f'{actor_key}.follow_camera'] = (
                    'true' if actor.get_follow_camera() else 'false'
                )
            if hasattr(actor, 'get_physics_enabled'):
                try:
                    self.file_data['actors'][f'{actor_key}.mechanics.physics_enabled'] = (
                        'true' if actor.get_physics_enabled() else 'false'
                    )
                except Exception:
                    pass

            # 获取几何体属性
            if hasattr(actor, '_geometry'):
                position = actor.get_position()
                rotation = actor.get_rotation()
                scale = actor.get_scale()

                self.file_data['actors'][
                    f'{actor_key}.geometry.position'] = _format_float3(position)
                self.file_data['actors'][
                    f'{actor_key}.geometry.rotation'] = _format_float3(rotation)
                self.file_data['actors'][
                    f'{actor_key}.geometry.scale'] = _format_float3(scale)

                # 持久化物理开关，使运行时关掉的物理能存进 .scene、F5 冷重载时读回。
                # 否则 INI 不带此字段 → 重载默认物理开启 → AI 摆放的物体互相穿插被
                # 求解器推得东歪西斜（甚至死循环）。只写显式取得到的值，取不到则不写
                # （保持向后兼容：老场景/无 mechanics 的 actor 行为不变）。
                if hasattr(actor, 'get_physics_enabled'):
                    try:
                        self.file_data['actors'][
                            f'{actor_key}.mechanics.physics_enabled'] = str(
                                bool(actor.get_physics_enabled())).lower()
                    except Exception:
                        pass

        # 脚本数据
        self.file_data['scripts']["path"] = getattr(self, 'script_path', '')

        # 地形数据
        self.file_data['terrain']["path"] = getattr(self, 'terrain_path', '')
        self.file_data['terrain']["type"] = getattr(self, 'terrain_type', '')

        vision_source_path = getattr(self, 'vision_source_path', '')
        vision_import_mode = getattr(self, 'vision_import_mode', '')
        if vision_source_path or vision_import_mode:
            if 'vision' not in self.file_data:
                self.file_data['vision'] = {}
            self.file_data['vision']['source_path'] = vision_source_path
            self.file_data['vision']['import_mode'] = vision_import_mode or 'external'

        self._write_indexed_section('vision_bindings', getattr(self, 'vision_bindings', []))
        self._write_indexed_section('vision_unsupported_shapes',
                                    getattr(self, 'vision_unsupported_shapes', []))

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
            CoronaEditor.js_call_func("scene-tree-changed", [scene_name])
        except Exception:
            pass

    @auto_save
    def add_actor(self, actor: Actor, rescene: bool = False) -> bool:
        if actor in self._actors and not rescene:
            return False
        # 自动处理同名冲突：追加后缀 _1, _2, ...
        if not rescene:
            existing_names = {a.name for a in self._actors}
            base_name = actor.name
            suffix = 1
            while actor.name in existing_names:
                actor.name = f"{base_name}_{suffix}"
                suffix += 1
            self._actors.append(actor)
        if hasattr(self.engine_scene, 'add_actor'):
            self.engine_scene.add_actor(actor.engine_obj)
        self._notify_scene_tree_changed()
        return True

    @auto_save
    def remove_actor(self, actor: Actor, rescene: bool = False) -> bool:
        if actor not in self._actors:
            return False
        should_broadcast_delete = not (
            getattr(actor, "network_remote", False) or
            getattr(actor, "_suppress_network_broadcast", False) or
            rescene
        )
        delete_payload = {
            "scene": self.route,
            "actor_guid": getattr(actor, "actor_guid", ""),
            "actor_name": getattr(actor, "name", ""),
        }
        del actor._optics
        if not rescene:
            self._actors.remove(actor)
        if hasattr(self.engine_scene, 'remove_actor'):
            self.engine_scene.remove_actor(actor.engine_obj)
        self._notify_scene_tree_changed()
        if should_broadcast_delete and (delete_payload["actor_guid"] or delete_payload["actor_name"]):
            try:
                CoronaEditor.js_call_func("actor-delete-sync-broadcast", [delete_payload])
            except Exception as exc:
                logger.warning("Actor delete network broadcast failed for %s: %s",
                               delete_payload["actor_guid"] or delete_payload["actor_name"], exc)
        return True  # 返回True触发auto_save

    @auto_save
    def clear_actors(self, rescene: bool = False) -> bool:
        for actor in self._actors.copy():
            self.remove_actor(actor, rescene)
        return True

    # 相机管理
    @auto_save
    def add_camera_to_scene(self, camera: Camera) -> bool:
        if camera in self._cameras:
            return False
        self._cameras.append(camera)
        self.engine_scene.add_camera(getattr(camera, 'engine_obj', camera))
        if self._main_camera is None:
            camera.deletable = False
            self._main_camera = camera
            if hasattr(self.engine_scene, 'set_active_camera'):
                self.engine_scene.set_active_camera(getattr(camera, 'engine_obj', camera))
        return True

    @auto_save
    def remove_camera_from_scene(self, camera: Camera) -> bool:
        if camera not in self._cameras:
            return False
        self._cameras.remove(camera)
        self.engine_scene.remove_camera(getattr(camera, 'engine_obj', camera))
        if self._main_camera is camera:
            self._main_camera = self._cameras[0] if self._cameras else None
            if self._main_camera is not None and hasattr(self.engine_scene, 'set_active_camera'):
                self.engine_scene.set_active_camera(getattr(self._main_camera, 'engine_obj', self._main_camera))
        return True

    @auto_save
    def clear_cameras(self) -> bool:
        for cam in self._cameras.copy():
            self.remove_camera_from_scene(cam)
        self._main_camera = None
        return True

    def get_cameras(self) -> List[Camera]:
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
            camera = Camera(name=f"{self.name}_MainCamera", deletable=False)
            self._cameras.append(camera)
            self.engine_scene.add_camera(getattr(camera, 'engine_obj', camera))
            self._main_camera = camera
            if hasattr(self.engine_scene, 'set_active_camera'):
                self.engine_scene.set_active_camera(getattr(camera, 'engine_obj', camera))
            created = True

        if self._main_camera is None:
            self._main_camera = self._cameras[0]
            self._main_camera.deletable = False
            if hasattr(self.engine_scene, 'set_active_camera'):
                self.engine_scene.set_active_camera(getattr(self._main_camera, 'engine_obj', self._main_camera))

        return created

    def get_active_camera(self) -> Optional[Camera]:
        self.ensure_default_camera()
        if not self._cameras:
            return None
        if self._main_camera is not None:
            return self._main_camera
        return self._cameras[0]

    def find_camera(self, camera_name: Optional[str]) -> Optional[Camera]:
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
            self._main_camera = camera if isinstance(camera, Camera) else self._main_camera
            if hasattr(self.engine_scene, 'set_active_camera'):
                self.engine_scene.set_active_camera(getattr(camera, 'engine_obj', camera))

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
        floor_grid_enabled = True
        env = self.get_environment()
        if env and hasattr(env, 'get_sun_direction'):
            sun_direction = env.get_sun_direction()
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
                "enabled": floor_grid_enabled,
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
                "source_path": self.vision_source_path,
                "import_mode": self.vision_import_mode,
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
        external_live = getattr(self, 'vision_import_mode', '') == 'external_live'
        for actor in getattr(self, '_actors', []):
            actor_guid = getattr(actor, 'actor_guid', '')
            binding = bindings_by_actor.get(actor_guid)
            if external_live and binding and hasattr(actor, 'set_external_vision_binding'):
                actor.set_external_vision_binding(binding)
            elif hasattr(actor, 'clear_external_vision_binding'):
                actor.clear_external_vision_binding()
