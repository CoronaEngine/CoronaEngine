from typing import Any, Dict
from .geometry import Geometry
from runtime.legacy_engine_adapter import CoronaEngine


class Acoustics:
    def __init__(self, geometry: Geometry, name: str = 'Acoustics', resource_id: int = 0):
        if CoronaEngine is None:
            raise RuntimeError('CoronaEngine 未初始化')
        AcousticsCtor = getattr(CoronaEngine, 'Acoustics', None)
        if AcousticsCtor is None:
            raise RuntimeError('CoronaEngine 未提供 Acoustics 构造器')
        geo_obj = geometry.engine_obj if hasattr(geometry, 'engine_obj') else geometry
        self.engine_obj = AcousticsCtor(geo_obj)
        self.name = name
        self.geometry = geometry
        self.resource_id = int(resource_id) if resource_id else 0
        if self.resource_id:
            self.engine_obj.set_audio_resource(self.resource_id)

    def set_volume(self, volume: float):
        self.engine_obj.set_volume(volume)

    def get_volume(self) -> float:
        return self.engine_obj.get_volume()

    def set_audio_resource(self, resource_id: int):
        self.resource_id = int(resource_id) if resource_id else 0
        self.engine_obj.set_audio_resource(self.resource_id)

    def get_audio_resource(self) -> int:
        return self.engine_obj.get_audio_resource()

    def play(self, loop: bool = False):
        self.engine_obj.play(bool(loop))

    def stop(self):
        self.engine_obj.stop()

    def to_dict(self) -> Dict[str, Any]:
        return {'name': self.name, 'resource_id': self.resource_id, 'engine_obj': self.engine_obj}

    def __repr__(self):
        return f'Acoustics(name={self.name}, resource_id={self.resource_id})'
