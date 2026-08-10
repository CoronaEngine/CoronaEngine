from abc import ABC, abstractmethod
from typing import Any, Dict, Optional
import logging


class ScriptBase(ABC):
    """脚本基类，所有脚本的抽象基类"""

    def __init__(self, name: str = ""):
        self.name = name
        self.is_initialized = False
        self.logger = logging.getLogger(f"{self.__class__.__name__}")

    @abstractmethod
    def initialize(self, *args, **kwargs):
        """初始化脚本"""
        pass

    @abstractmethod
    def update(self, delta_time: float):
        """每帧更新"""
        pass

    def shutdown(self):
        """关闭脚本"""
        self.on_shutdown()
        self.is_initialized = False
        self.logger.info(f"Script {self.name} shutdown")

    def on_start(self) -> None:
        pass

    def on_update(self, delta_time: float) -> None:
        pass

    def on_shutdown(self) -> None:
        pass

    def on_event(self, event_name: str, data: Any = None) -> None:
        """处理事件"""
        pass

    def to_dict(self) -> Dict[str, Any]:
        """序列化"""
        return {
            "name": self.name,
            "type": self.__class__.__name__,
            "is_initialized": self.is_initialized
        }
