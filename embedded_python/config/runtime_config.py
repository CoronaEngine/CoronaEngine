"""
运行时配置

字段只保留宿主真正读取的值：`log_level` 决定根 logger 级别，`enable_gpu` / `debug_mode`
是 `CORONA_*` 环境变量暴露的开关。InnerAgent 的三个字段没有任何消费者，已删除。
"""
from dataclasses import dataclass, fields
from typing import Any, Dict


@dataclass(frozen=True)
class RuntimeConfig:
    """运行时配置"""
    enable_gpu: bool = False
    log_level: str = "INFO"
    debug_mode: bool = False

    @classmethod
    def get_defaults(cls) -> Dict[str, Any]:
        """获取所有字段的默认值"""
        from dataclasses import MISSING
        return {
            f.name: f.default
            for f in fields(cls)
            if f.default is not MISSING
        }
