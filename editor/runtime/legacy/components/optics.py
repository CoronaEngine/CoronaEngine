from typing import Any, Dict
from .geometry import Geometry

from runtime.legacy_engine_adapter import CoronaEngine


class Optics:
    def __init__(self, geometry: Geometry, name: str = 'Optics'):
        if CoronaEngine is None:
            raise RuntimeError('CoronaEngine 未初始化')
        OpticsCtor = getattr(CoronaEngine, 'Optics', None)
        if OpticsCtor is None:
            raise RuntimeError('CoronaEngine 未提供 Optics 构造器')
        geo_obj = geometry.engine_obj if hasattr(geometry, 'engine_obj') else geometry
        self.engine_obj = OpticsCtor(geo_obj)
        self.name = name
        self.geometry = geometry

    def set_metallic(self, value: float): self.engine_obj.set_metallic(value)
    def get_metallic(self) -> float: return self.engine_obj.get_metallic()
    def set_roughness(self, value: float): self.engine_obj.set_roughness(value)
    def get_roughness(self) -> float: return self.engine_obj.get_roughness()
    def set_subsurface(self, value: float): self.engine_obj.set_subsurface(value)
    def get_subsurface(self) -> float: return self.engine_obj.get_subsurface()
    def set_specular(self, value: float): self.engine_obj.set_specular(value)
    def get_specular(self) -> float: return self.engine_obj.get_specular()
    def set_specular_tint(self, value: float): self.engine_obj.set_specular_tint(value)
    def get_specular_tint(self) -> float: return self.engine_obj.get_specular_tint()
    def set_anisotropic(self, value: float): self.engine_obj.set_anisotropic(value)
    def get_anisotropic(self) -> float: return self.engine_obj.get_anisotropic()
    def set_sheen(self, value: float): self.engine_obj.set_sheen(value)
    def get_sheen(self) -> float: return self.engine_obj.get_sheen()
    def set_sheen_tint(self, value: float): self.engine_obj.set_sheen_tint(value)
    def get_sheen_tint(self) -> float: return self.engine_obj.get_sheen_tint()
    def set_clearcoat(self, value: float): self.engine_obj.set_clearcoat(value)
    def get_clearcoat(self) -> float: return self.engine_obj.get_clearcoat()
    def set_clearcoat_gloss(self, value: float): self.engine_obj.set_clearcoat_gloss(value)
    def get_clearcoat_gloss(self) -> float: return self.engine_obj.get_clearcoat_gloss()
    def set_visible(self, visible: bool): self.engine_obj.set_visible(visible)
    def get_visible(self) -> bool: return self.engine_obj.get_visible()
    def set_ambient(self, value: list[float]): self.engine_obj.set_ambient(value)
    def get_ambient(self) -> list[float]: return self.engine_obj.get_ambient()
    def set_diffuse(self, value: list[float]): self.engine_obj.set_diffuse(value)
    def get_diffuse(self) -> list[float]: return self.engine_obj.get_diffuse()
    def set_specular_color(self, value: list[float]): self.engine_obj.set_specular_color(value)
    def get_specular_color(self) -> list[float]: return self.engine_obj.get_specular_color()
    def set_shininess(self, value: float): self.engine_obj.set_shininess(value)
    def get_shininess(self) -> float: return self.engine_obj.get_shininess()

    def set_lighting_enabled(self, enabled: bool): self.engine_obj.set_lighting_enabled(enabled)
    def get_lighting_enabled(self) -> bool: return self.engine_obj.get_lighting_enabled()

    def to_dict(self) -> Dict[str, Any]:
        return {
            'name': self.name,
            'metallic': self.get_metallic(),
            'roughness': self.get_roughness(),
            'subsurface': self.get_subsurface(),
            'specular': self.get_specular(),
            'specular_tint': self.get_specular_tint(),
            'anisotropic': self.get_anisotropic(),
            'sheen': self.get_sheen(),
            'sheen_tint': self.get_sheen_tint(),
            'clearcoat': self.get_clearcoat(),
            'clearcoat_gloss': self.get_clearcoat_gloss(),
            'visible': self.get_visible(),
            'ambient': list(self.get_ambient()),
            'diffuse': list(self.get_diffuse()),
            'specular_color': list(self.get_specular_color()),
            'shininess': self.get_shininess(),
        }

    def __repr__(self):
        return f'Optics(name={self.name})'
