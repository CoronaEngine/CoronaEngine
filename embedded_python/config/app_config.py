"""
应用全局配置
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, Optional
import os

from .runtime_config import RuntimeConfig
from .paths_config import PathsConfig, get_default_paths

_CACHE: Optional["AppConfig"] = None



@dataclass(frozen=True)
class AppConfig:
    """应用全局配置"""
    runtime: RuntimeConfig
    paths: PathsConfig


def _apply_env_overrides(data: Dict[str, Any]) -> None:
    """应用环境变量覆盖"""
    overrides = {
        ("runtime", "enable_gpu"): os.getenv("CORONA_ENABLE_GPU"),
        ("runtime", "log_level"): os.getenv("CORONA_LOG_LEVEL"),
        ("runtime", "debug_mode"): os.getenv("CORONA_DEBUG"),
    }
    for path, value in overrides.items():
        if value is None:
            continue
        section = data
        for part in path[:-1]:
            section = section.setdefault(part, {})
        key = path[-1]
        if key in {"enable_gpu", "debug_mode"}:
            section[key] = str(value).strip().lower() in {"1", "true", "yes", "on"}
        else:
            section[key] = value


def _load_config_data() -> Dict[str, Any]:
    """加载配置数据(优先级:环境变量 > 用户配置 > 项目配置 > dataclass默认值)"""
    # 1. 从 dataclass 默认值开始
    merged = {"runtime": RuntimeConfig.get_defaults()}

    # 4. 应用环境变量覆盖
    _apply_env_overrides(merged)

    return merged


def _as_bool(value: Any, default: bool) -> bool:
    """转换为布尔值"""
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    return str(value).strip().lower() in {"1", "true", "yes", "on"}


# ---------------------------------------------------------------------------
# 公共函数
# ---------------------------------------------------------------------------


def _build_app_config() -> AppConfig:
    """构建应用配置"""
    raw = _load_config_data()

    # 加载路径配置
    paths = get_default_paths()

    # 加载运行时配置
    runtime_data = raw.get("runtime", {})
    defaults = RuntimeConfig.get_defaults()
    runtime = RuntimeConfig(
        enable_gpu=_as_bool(runtime_data.get("enable_gpu"), defaults["enable_gpu"]),
        log_level=str(runtime_data.get("log_level", defaults["log_level"])).upper(),
        debug_mode=_as_bool(runtime_data.get("debug_mode"), defaults["debug_mode"]),
        InnerAgentWorkFlow=_as_bool(runtime_data.get("InnerAgentWorkFlow"), defaults["InnerAgentWorkFlow"]),
        InnerAgentRepoUrl=str(runtime_data.get("InnerAgentRepoUrl", defaults["InnerAgentRepoUrl"])),
        InnerAgentTargetDir=str(runtime_data.get("InnerAgentTargetDir", defaults["InnerAgentTargetDir"])),
    )

    return AppConfig(
        runtime=runtime,
        paths=paths,
    )


def get_app_config() -> AppConfig:
    """获取应用配置(单例)"""
    global _CACHE
    if _CACHE is None:
        _CACHE = _build_app_config()
    return _CACHE


def reload_app_config() -> AppConfig:
    """重新加载应用配置"""
    global _CACHE
    _CACHE = _build_app_config()
    return _CACHE


__all__ = [
    "AppConfig",
    "get_app_config",
    "reload_app_config",
]
