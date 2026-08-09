"""
运行时配置
"""
from dataclasses import dataclass, fields
from typing import Any, Dict


@dataclass(frozen=True)
class RuntimeConfig:
    """运行时配置"""
    enable_gpu: bool = False
    log_level: str = "INFO"
    debug_mode: bool = False
    InnerAgentWorkFlow: bool = False
    InnerAgentRepoUrl: str = "https://github.com/CoronaEngine/InnerAgentWorkflow.git"
    InnerAgentTargetDir: str = "./InnerAgentWorkflow"

    @classmethod
    def get_defaults(cls) -> Dict[str, Any]:
        """获取所有字段的默认值"""
        from dataclasses import MISSING
        return {
            f.name: f.default
            for f in fields(cls)
            if f.default is not MISSING
        }
