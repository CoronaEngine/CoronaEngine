from ..script_base import ScriptBase
from typing import Dict, Any, Optional
import json
import os


class ProjectScript(ScriptBase):
    """
    总脚本：存放玩家数据、在启动项目时初始化
    生命周期：项目启动 -> 项目关闭
    作用范围：整个项目全局
    """

    def __init__(self, name: str = "GlobalScript"):
        super().__init__(name)

    def initialize(self, *args, **kwargs):
        """
        初始化全局脚本
        Args:
            project_path: 项目路径
        """
        pass

    def update(self, delta_time: float):
        """全局更新（通常不需要每帧更新）"""
        pass


    def shutdown(self) -> None:
        """关闭全局脚本"""
        super().shutdown()
