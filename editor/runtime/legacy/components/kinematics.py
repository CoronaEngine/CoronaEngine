from typing import Any, Dict
from .geometry import Geometry

from runtime.legacy_engine_adapter import CoronaEngine


class Kinematics:
    def __init__(self, geometry: Geometry, name: str = 'Kinematics'):
        if CoronaEngine is None:
            raise RuntimeError('CoronaEngine 未初始化')
        KinematicsCtor = getattr(CoronaEngine, 'Kinematics', None)
        if KinematicsCtor is None:
            raise RuntimeError('CoronaEngine 未提供 Kinematics 构造器')
        geo_obj = geometry.engine_obj if hasattr(geometry, 'engine_obj') else geometry
        self.engine_obj = KinematicsCtor(geo_obj)
        self.name = name
        self.geometry = geometry

    def set_animation(self, animation_index: int):
        self.engine_obj.set_animation(animation_index)

    def play_animation(self, speed: float = 1.0):
        self.engine_obj.play_animation(speed)

    def stop_animation(self):
        self.engine_obj.stop_animation()

    def get_animation_index(self) -> int:
        return self.engine_obj.get_animation_index()

    def get_current_time(self) -> float:
        return self.engine_obj.get_current_time()

    def to_dict(self) -> Dict[str, Any]:
        return {'name': self.name, 'engine_obj': self.engine_obj}

    def __repr__(self):
        return f'Kinematics(name={self.name})'
