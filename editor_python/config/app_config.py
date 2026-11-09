"""
应用全局配置
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Mapping, Optional
import os
import tomllib
import configparser

from .runtime_config import RuntimeConfig
from .paths_config import PathsConfig, get_default_paths

_CACHE: Optional["AppConfig"] = None



@dataclass(frozen=True)
class AppConfig:
    """应用全局配置"""
    runtime: RuntimeConfig
    paths: PathsConfig


# ---------------------------------------------------------------------------
# 文件加载辅助函数
# ---------------------------------------------------------------------------


def _load_toml(path: Path) -> Dict[str, Any]:
    """加载 TOML 文件"""
    if not path.exists():
        return {}
    try:
        return tomllib.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"加载 TOML 配置失败 {path}: {e}")
        return {}


def _load_ini(path: Path) -> Dict[str, Any]:
    """加载 INI 文件"""
    if not path.exists():
        return {}
    try:
        config = configparser.ConfigParser()
        config.read(path, encoding="utf-8")

        result = {}

        # 解析 runtime section
        if "runtime" in config:
            result["runtime"] = {}
            if "enable_gpu" in config["runtime"]:
                result["runtime"]["enable_gpu"] = config["runtime"].getboolean("enable_gpu")
            if "log_level" in config["runtime"]:
                result["runtime"]["log_level"] = config["runtime"]["log_level"]
            if "debug_mode" in config["runtime"]:
                result["runtime"]["debug_mode"] = config["runtime"].getboolean("debug_mode")

        return result
    except Exception as e:
        print(f"加载 INI 配置失败 {path}: {e}")
        return {}


def _deep_merge(base: Dict[str, Any], override: Mapping[str, Any]) -> Dict[str, Any]:
    """深度合并字典"""
    result = dict(base)
    for key, value in override.items():
        if isinstance(value, Mapping) and isinstance(result.get(key), Mapping):
            result[key] = _deep_merge(result[key], value)
        else:
            result[key] = value
    return result


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
