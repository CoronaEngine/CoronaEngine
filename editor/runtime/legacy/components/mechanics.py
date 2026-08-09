from typing import Any, Dict
from .geometry import Geometry

from runtime.legacy_engine_adapter import CoronaEngine


class Mechanics:
    def __init__(self, geometry: Geometry, name: str = 'Mechanics'):
        if CoronaEngine is None:
            raise RuntimeError('CoronaEngine 未初始化')
        MechanicsCtor = getattr(CoronaEngine, 'Mechanics', None)
        if MechanicsCtor is None:
            raise RuntimeError('CoronaEngine 未提供 Mechanics 构造器')
        geo_obj = geometry.engine_obj if hasattr(geometry, 'engine_obj') else geometry
        self.engine_obj = MechanicsCtor(geo_obj)
        self.name = name
        self.geometry = geometry

    def set_mass(self, mass: float):
        try:
            self.engine_obj.set_mass(mass)
        except Exception as e:
            raise RuntimeError(f"Mechanics.set_mass 失败: {e}") from e

    def get_mass(self) -> float:
        try:
            return self.engine_obj.get_mass()
        except Exception as e:
            raise RuntimeError(f"Mechanics.get_mass 失败: {e}") from e

    def set_restitution(self, restitution: float):
        try:
            self.engine_obj.set_restitution(restitution)
        except Exception as e:
            raise RuntimeError(f"Mechanics.set_restitution 失败: {e}") from e

    def get_restitution(self) -> float:
        try:
            return self.engine_obj.get_restitution()
        except Exception as e:
            raise RuntimeError(f"Mechanics.get_restitution 失败: {e}") from e

    def set_damping(self, damping: float):
        try:
            self.engine_obj.set_damping(damping)
        except Exception as e:
            raise RuntimeError(f"Mechanics.set_damping 失败: {e}") from e

    def get_damping(self) -> float:
        try:
            return self.engine_obj.get_damping()
        except Exception as e:
            raise RuntimeError(f"Mechanics.get_damping 失败: {e}") from e

    def set_physics_enabled(self, enabled: bool):
        try:
            self.engine_obj.set_physics_enabled(enabled)
        except Exception as e:
            raise RuntimeError(f"Mechanics.set_physics_enabled 失败: {e}") from e

    def get_physics_enabled(self) -> bool:
        try:
            return self.engine_obj.get_physics_enabled()
        except Exception as e:
            raise RuntimeError(f"Mechanics.get_physics_enabled 失败: {e}") from e

    def set_collision_enabled(self, enabled: bool):
        """启用或禁用碰撞检测（关闭后物体不参与碰撞，也不受地板碰撞）"""
        try:
            self.engine_obj.set_collision_enabled(enabled)
        except Exception as e:
            raise RuntimeError(f"Mechanics.set_collision_enabled 失败: {e}") from e

    def get_collision_enabled(self) -> bool:
        """获取碰撞检测开关状态"""
        try:
            return self.engine_obj.get_collision_enabled()
        except Exception as e:
            raise RuntimeError(f"Mechanics.get_collision_enabled 失败: {e}") from e

    def set_collision_shape(self, shape: str):
        self.engine_obj.set_collision_shape(shape)

    def get_collision_shape(self) -> str:
        return self.engine_obj.get_collision_shape()

    def set_linear_lock(self, lock_x: bool, lock_y: bool, lock_z: bool):
        """设置线性运动轴锁定（平移锁定）"""
        try:
            self.engine_obj.set_linear_lock(lock_x, lock_y, lock_z)
        except Exception as e:
            raise RuntimeError(f"Mechanics.set_linear_lock 失败: {e}") from e

    def get_linear_lock(self):
        """获取线性运动轴锁定状态，返回 (lock_x, lock_y, lock_z)"""
        try:
            return self.engine_obj.get_linear_lock()
        except Exception as e:
            raise RuntimeError(f"Mechanics.get_linear_lock 失败: {e}") from e

    def set_angular_lock(self, lock_x: bool, lock_y: bool, lock_z: bool):
        """设置角度运动轴锁定（旋转锁定）"""
        try:
            self.engine_obj.set_angular_lock(lock_x, lock_y, lock_z)
        except Exception as e:
            raise RuntimeError(f"Mechanics.set_angular_lock 失败: {e}") from e

    def get_angular_lock(self):
        """获取角度运动轴锁定状态，返回 (lock_x, lock_y, lock_z)"""
        try:
            return self.engine_obj.get_angular_lock()
        except Exception as e:
            raise RuntimeError(f"Mechanics.get_angular_lock 失败: {e}") from e

    def set_collision_callback(self, callback):
        """
        设置碰撞回调
        callback: 函数，签名为 (other_handle, normal_x, normal_y, normal_z, point_x, point_y, point_z)
        注意：other_handle是整数，需要在用户层转换为Actor对象（如果必要）
        """
        self.engine_obj.set_collision_callback(callback)

    def set_on_move_callback(self, callback):
        """
        设置碰撞回调
        callback: 函数，签名为 (other_handle, normal_x, normal_y, normal_z, point_x, point_y, point_z)
        注意：other_handle是整数，需要在用户层转换为Actor对象（如果必要）
        """
        self.engine_obj.set_on_move_callback(callback)

    def to_dict(self) -> Dict[str, Any]:
        result = {'name': self.name}
        try:
            result['mass'] = self.get_mass()
            result['restitution'] = self.get_restitution()
            result['damping'] = self.get_damping()
            result['physics_enabled'] = self.get_physics_enabled()
            linear = self.get_linear_lock()
            result['linear_lock'] = list(linear)
            angular = self.get_angular_lock()
            result['angular_lock'] = list(angular)
        except Exception:
            pass
        return result

    def __repr__(self):
        return f'Mechanics(name={self.name})'
