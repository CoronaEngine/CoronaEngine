"""
Components - 游戏对象的组件
所有组件都依赖 Geometry，提供不同的功能（渲染、物理、动画、声音）
"""

from .geometry import Geometry
from .optics import Optics
from .mechanics import Mechanics
from .kinematics import Kinematics
from .acoustics import Acoustics

__all__ = [
    "Geometry",
    "Optics",
    "Mechanics",
    "Kinematics",
    "Acoustics",
]
