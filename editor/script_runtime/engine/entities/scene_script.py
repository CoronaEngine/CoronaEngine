from __future__ import annotations

from typing import Optional

from ..script_base import ScriptBase
from ..contracts import SceneScriptTarget


class SceneScript(ScriptBase):
    """
    场景脚本：存放场景数据，在切换场景时初始化
    生命周期：场景加载 -> 场景卸载
    作用范围：当前场景
    """

    def __init__(self, name: str = "SceneScript", scene: SceneScriptTarget = None):
        super().__init__(name)
        self.scene: Optional[SceneScriptTarget] = scene

    def initialize(self, *args, **kwargs):
        """
        初始化场景脚本
        Args:
            scene: 关联的场景对象
            scene_data: 场景数据（可选）
        """
        pass

    def update(self, delta_time: float):
        """场景更新"""
        pass
