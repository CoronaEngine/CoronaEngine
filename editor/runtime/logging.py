from __future__ import annotations

import logging
import os
import sys
import traceback
from typing import Optional

from config.app_config import get_app_config

try:
    from runtime.native_engine import get_corona_engine

    CoronaEngine = get_corona_engine()
except Exception:
    CoronaEngine = None

_CONFIGURED = False

# Python -> C++ 级别映射。C++ 接受 TRACE/DEBUG/INFO/WARNING/ERROR/CRITICAL/NOTICE
_LEVEL_MAP = {
    "TRACE": "TRACE",
    "DEBUG": "DEBUG",
    "INFO": "INFO",
    "WARNING": "WARNING",
    "ERROR": "ERROR",
    "CRITICAL": "CRITICAL",
    "NOTICE": "NOTICE",
}

# 默认收敛的第三方 SDK 日志噪声。键是 logger 名，值是阈值。
_DEFAULT_QUIET_LOGGERS = {
    "openai": logging.WARNING,
    "openai._base_client": logging.WARNING,
    "httpx": logging.WARNING,
    "httpcore": logging.WARNING,
    "urllib3": logging.WARNING,
    "asyncio": logging.WARNING,
    "PIL": logging.WARNING,
    "matplotlib": logging.WARNING,
}

# 即便根 logger 是 INFO，也希望这些业务模块默认 DEBUG 起来便于排查。
# 可以通过环境变量 CORONA_VERBOSE_LOGGERS=name1=DEBUG,name2=INFO 覆盖。
_DEFAULT_VERBOSE_LOGGERS = {
    "ai_modules": logging.DEBUG,
    "ai_workflow": logging.DEBUG,
    "ai_tools": logging.DEBUG,
    "ai_agent": logging.DEBUG,
    "ai_service": logging.INFO,
    "cai_extensions": logging.DEBUG,
    "plugins.AITool": logging.DEBUG,
}


class CppLoggingHandler(logging.Handler):
    """将 Python logging 转发到 C++ spdlog（CoronaEngine.send_log）。"""

    def emit(self, record: logging.LogRecord) -> None:
        try:
            msg = self.format(record)
            level_name = _LEVEL_MAP.get(record.levelname, "INFO")
            if CoronaEngine is not None and hasattr(CoronaEngine, "send_log"):
                CoronaEngine.send_log(level_name, msg)
        except Exception:
            # 走 Handler 内置错误流程，避免吞异常
            self.handleError(record)


class _StreamToLogger:
    """把 sys.stdout/sys.stderr 重定向到 logging。

    只在按行边界落日志，避免把 print 的逗号分隔片段拆得太碎。
    """

    def __init__(self, logger: logging.Logger, level: int) -> None:
        self._logger = logger
        self._level = level
        self._buffer = ""

    def write(self, text: str) -> int:  # type: ignore[override]
        if not isinstance(text, str):
            try:
                text = str(text)
            except Exception:
                return 0
        if not text:
            return 0
        self._buffer += text
        while "\n" in self._buffer:
            line, self._buffer = self._buffer.split("\n", 1)
            line = line.rstrip("\r")
            if line.strip():
                self._logger.log(self._level, line)
        return len(text)

    def flush(self) -> None:
        if self._buffer.strip():
            self._logger.log(self._level, self._buffer.rstrip())
        self._buffer = ""

    # 兼容部分库会读取这些属性
    def isatty(self) -> bool:
        return False

    @property
    def encoding(self) -> str:
        return "utf-8"

    def writable(self) -> bool:
        return True


def _install_stream_redirect() -> None:
    """把 stdout/stderr 接管到 logging。"""
    sys.stdout = _StreamToLogger(logging.getLogger("stdout"), logging.INFO)
    sys.stderr = _StreamToLogger(logging.getLogger("stderr"), logging.ERROR)


def _apply_logger_levels(spec: dict[str, int]) -> None:
    for name, level in spec.items():
        logging.getLogger(name).setLevel(level)


def _parse_env_logger_overrides(env_value: Optional[str]) -> dict[str, int]:
    """解析 ``name1=DEBUG,name2=INFO`` 形式的环境变量。"""
    if not env_value:
        return {}
    out: dict[str, int] = {}
    for item in env_value.split(","):
        item = item.strip()
        if not item or "=" not in item:
            continue
        name, _, level_str = item.partition("=")
        name, level_str = name.strip(), level_str.strip().upper()
        if not name:
            continue
        level = getattr(logging, level_str, None)
        if isinstance(level, int):
            out[name] = level
    return out


def configure_logging() -> None:
    """初始化日志系统（幂等）。

    如果配置过程出错，会把异常打到 stderr/INI fallback 上而非静默吞掉。
    """
    global _CONFIGURED
    if _CONFIGURED:
        return

    try:
        config = get_app_config()
        level_name = getattr(config.runtime, "log_level", "INFO") or "INFO"
        level = getattr(logging, level_name.upper(), logging.INFO)

        root_logger = logging.getLogger()
        root_logger.setLevel(level)

        if CoronaEngine:
            root_logger.handlers.clear()

            cpp_handler = CppLoggingHandler()
            # Handler 自身放最低级别，让 logger 级别决定过滤
            cpp_handler.setLevel(logging.DEBUG)
            cpp_handler.setFormatter(
                logging.Formatter(
                    "%(name)s [%(filename)s:%(lineno)d] [%(threadName)s] %(message)s"
                )
            )
            root_logger.addHandler(cpp_handler)

            # 接管 print/stderr，只在 C++ 桥接通时才做，否则会把控制台输出也吞掉
            _install_stream_redirect()
        else:
            logging.basicConfig(
                level=level,
                format="%(asctime)s [%(levelname)s] %(name)s [%(filename)s:%(lineno)d] [%(threadName)s] %(message)s",
                force=True,
            )

        _apply_logger_levels(_DEFAULT_QUIET_LOGGERS)
        _apply_logger_levels(_DEFAULT_VERBOSE_LOGGERS)
        _apply_logger_levels(_parse_env_logger_overrides(os.getenv("CORONA_VERBOSE_LOGGERS")))

        _CONFIGURED = True
    except Exception:
        # 任何失败都明确暴露，避免 main.py try/except 把它吞了什么都看不到
        try:
            sys.__stderr__.write(
                "[runtime.logging] configure_logging failed:\n" + traceback.format_exc()
            )
            sys.__stderr__.flush()
        except Exception:
            pass
        raise


def get_logger(name: str) -> logging.Logger:
    if not _CONFIGURED:
        try:
            configure_logging()
        except Exception:
            pass
    return logging.getLogger(name)
