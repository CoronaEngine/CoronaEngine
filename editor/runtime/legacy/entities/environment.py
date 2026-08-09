from typing import Any, Dict, List

from runtime.legacy_engine_adapter import CoronaEngine


class Environment:
    """
    Environment 包装类：环境设置（太阳方向、地面网格等），纯透传 C++ EnvironmentDevice。
    """

    def __init__(self, name: str = "Environment"):
        if CoronaEngine is None:
            raise RuntimeError("CoronaEngine 未初始化")

        EnvironmentCtor = getattr(CoronaEngine, 'Environment', None)
        if EnvironmentCtor is None:
            raise RuntimeError("CoronaEngine 未提供 Environment 构造器")

        self.engine_obj = EnvironmentCtor()
        self.name = name

    # ---- 太阳方向 ----
    def set_sun_direction(self, direction: List[float]):
        self.engine_obj.set_sun_direction(direction)

    def get_sun_direction(self) -> List[float]:
        return list(self.engine_obj.get_sun_direction())

    # ---- 地面网格 ----
    def set_sun_intensity(self, intensity: float):
        if hasattr(self.engine_obj, 'set_sun_intensity'):
            self.engine_obj.set_sun_intensity(float(intensity))

    def get_sun_intensity(self) -> float:
        if hasattr(self.engine_obj, 'get_sun_intensity'):
            return float(self.engine_obj.get_sun_intensity())
        return 10.0

    def set_sky_intensity(self, intensity: float):
        if hasattr(self.engine_obj, 'set_sky_intensity'):
            self.engine_obj.set_sky_intensity(float(intensity))

    def get_sky_intensity(self) -> float:
        if hasattr(self.engine_obj, 'get_sky_intensity'):
            return float(self.engine_obj.get_sky_intensity())
        return 20.0

    def set_floor_grid(self, enabled: bool):
        self.engine_obj.set_floor_grid(enabled)

    def get_floor_grid(self) -> bool:
        return self.engine_obj.get_floor_grid()

    # ---- 重力 ----
    def set_gravity(self, gravity: List[float]):
        self.engine_obj.set_gravity(gravity)

    def get_gravity(self) -> List[float]:
        return list(self.engine_obj.get_gravity())

    # ---- 地面高度 ----
    def set_floor_y(self, y: float):
        self.engine_obj.set_floor_y(y)

    def get_floor_y(self) -> float:
        return self.engine_obj.get_floor_y()

    # ---- 地面弹性系数 ----
    def set_floor_restitution(self, restitution: float):
        self.engine_obj.set_floor_restitution(restitution)

    def get_floor_restitution(self) -> float:
        return self.engine_obj.get_floor_restitution()

    # ---- 物理固定时间步长 ----
    def set_fixed_dt(self, dt: float):
        self.engine_obj.set_fixed_dt(dt)

    def get_fixed_dt(self) -> float:
        return self.engine_obj.get_fixed_dt()

    def to_dict(self) -> Dict[str, Any]:
        return {
            'name': self.name,
            'sun_direction': list(self.engine_obj.get_sun_direction()),
            'sun_intensity': self.get_sun_intensity(),
            'sky_intensity': self.get_sky_intensity(),
            'floor_grid_enabled': self.engine_obj.get_floor_grid(),
            'gravity': list(self.engine_obj.get_gravity()),
            'floor_y': self.engine_obj.get_floor_y(),
            'floor_restitution': self.engine_obj.get_floor_restitution(),
            'fixed_dt': self.engine_obj.get_fixed_dt(),
            'engine_obj': self.engine_obj,
        }

    def __repr__(self):
        return f"Environment(name={self.name})"
