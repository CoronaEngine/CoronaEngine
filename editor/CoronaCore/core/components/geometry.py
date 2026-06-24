from typing import Any, Dict, List


from ..corona_editor import CoronaEditor

CoronaEngine = CoronaEditor.CoronaEngine


class Geometry:
    """
    Geometry 包装类：几何体，存储模型数据和变换信息（位置/旋转/缩放）

    使用方式：
        geo = Geometry("assets/model/character.obj")
        geo.set_position([0, 0, 0])
        geo.set_rotation([0, 0, 0])
    """

    def __init__(self, model_path: str, name: str = "Geometry"):
        """
        创建 Geometry 对象

        Args:
            model_path: 模型文件路径
            name: 几何体名称
        """
        if CoronaEngine is None:
            raise RuntimeError("CoronaEngine 未初始化")

        GeometryCtor = getattr(CoronaEngine, 'Geometry', None)
        if GeometryCtor is None:
            raise RuntimeError("CoronaEngine 未提供 Geometry 构造器")

        self.engine_obj = GeometryCtor(model_path)
        self.name = name
        self.model_path = model_path
        self.is_ui_image = False

    @classmethod
    def from_image(cls, image_path: str, name: str = "UIImage"):
        """
        从图片文件创建一个带贴图的 quad（两个三角形）几何，用作光场 UI 平面。

        与普通模型 Geometry 不同：底层走 C++ Geometry.from_image，程序化生成顶点并
        把图片直接作为 albedo texture 上传，不经 Resource::Scene。

        Args:
            image_path: 图片文件路径（png/jpg/...）
            name: 几何体名称
        """
        if CoronaEngine is None:
            raise RuntimeError("CoronaEngine 未初始化")

        GeometryCtor = getattr(CoronaEngine, 'Geometry', None)
        if GeometryCtor is None or not hasattr(GeometryCtor, 'from_image'):
            raise RuntimeError("CoronaEngine 未提供 Geometry.from_image")

        obj = cls.__new__(cls)
        obj.engine_obj = GeometryCtor.from_image(image_path)
        obj.name = name
        obj.model_path = image_path
        obj.is_ui_image = True
        return obj

    def set_position(self, position: List[float]):
        """设置局部位置 [x, y, z]"""
        try:
            self.engine_obj.set_position(position)
        except Exception as e:
            raise RuntimeError(f"Geometry.set_position 失败: {e}") from e

    def get_position(self) -> List[float]:
        """获取局部位置 [x, y, z]"""
        try:
            return self.engine_obj.get_position()
        except Exception as e:
            raise RuntimeError(f"Geometry.get_position 失败: {e}") from e

    def set_rotation(self, euler: List[float]):
        """设置局部旋转（欧拉角 ZYX 顺序）[pitch, yaw, roll]"""
        try:
            self.engine_obj.set_rotation(euler)
        except Exception as e:
            raise RuntimeError(f"Geometry.set_rotation 失败: {e}") from e

    def get_rotation(self) -> List[float]:
        """获取局部旋转（欧拉角）[pitch, yaw, roll]"""
        try:
            return self.engine_obj.get_rotation()
        except Exception as e:
            raise RuntimeError(f"Geometry.get_rotation 失败: {e}") from e

    def set_scale(self, scale: List[float]):
        """设置局部缩放 [x, y, z]"""
        try:
            self.engine_obj.set_scale(scale)
        except Exception as e:
            raise RuntimeError(f"Geometry.set_scale 失败: {e}") from e

    def set_native_local_correction(self, offset: List[float], scale: float):
        """设置仅 native 渲染使用的本地几何校正。"""
        setter = getattr(self.engine_obj, 'set_native_local_correction', None)
        if not callable(setter):
            return
        try:
            setter(offset, float(scale))
        except Exception as e:
            raise RuntimeError(f"Geometry.set_native_local_correction 失败: {e}") from e

    def get_scale(self) -> List[float]:
        """获取局部缩放 [x, y, z]"""
        try:
            return self.engine_obj.get_scale()
        except Exception as e:
            raise RuntimeError(f"Geometry.get_scale 失败: {e}") from e

    def get_aabb(self) -> List[float]:
        """获取模型 AABB [min_x, min_y, min_z, max_x, max_y, max_z]"""
        try:
            return list(self.engine_obj.get_aabb())
        except Exception as e:
            raise RuntimeError(f"Geometry.get_aabb 失败: {e}") from e

    def to_dict(self) -> Dict[str, Any]:
        """转换为字典表示"""
        return {
            'name': self.name,
            'model_path': self.model_path,
            'engine_obj': self.engine_obj,
        }

    def __repr__(self):
        return f"Geometry(name={self.name}, path={self.model_path})"
