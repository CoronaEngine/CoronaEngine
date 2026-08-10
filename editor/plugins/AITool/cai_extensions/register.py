"""CabbageEditor adapter for CAI.

This module keeps the physical ``cai_extensions`` location stable while exposing
host capabilities as CAI runtime plugins. The old ``install()`` entry remains
compatible and installs the adapter into the default CAI app.
"""

from __future__ import annotations

import logging
import threading
from dataclasses import dataclass
from importlib import import_module
from pathlib import Path
from typing import Iterable

logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class CabbageContext:
    aitool_dir: Path
    cai_dir: Path

    @classmethod
    def from_default_locations(cls) -> "CabbageContext":
        aitool_dir = Path(__file__).resolve().parents[1]
        return cls(
            aitool_dir=aitool_dir,
            cai_dir=aitool_dir / "Quasar",
        )


def bootstrap_paths(context: CabbageContext | None = None) -> CabbageContext:
    return context or CabbageContext.from_default_locations()


class CabbagePathsPlugin:
    name = "cabbage.paths"
    enabled = True

    def __init__(self, context: CabbageContext):
        self.context = context

    def register(self, runtime) -> dict:
        from Quasar.ai_config.paths_config import set_paths_resolver

        from .paths_provider import CabbageEditorPathsResolver

        resolver = CabbageEditorPathsResolver()
        runtime.set_capability("paths_resolver", resolver)
        set_paths_resolver(resolver)
        runtime.metadata.setdefault("cabbage_adapter", {})[self.name] = True
        logger.debug("[cai_extensions] paths resolver installed")
        return {"name": self.name}


class CabbageAppConfigPlugin:
    name = "cabbage.app_config"
    enabled = True

    def __init__(self, context: CabbageContext):
        self.context = context

    def register(self, runtime) -> dict:
        from Quasar.ai_tools.warmup import set_app_config_provider

        from .app_config_provider import get_app_config_for_cai

        runtime.set_capability("app_config_provider", get_app_config_for_cai)
        set_app_config_provider(get_app_config_for_cai)
        runtime.metadata.setdefault("cabbage_adapter", {})[self.name] = True
        logger.debug("[cai_extensions] app_config provider installed")
        return {"name": self.name}


class CabbageEngineToolsPlugin:
    name = "cabbage.engine_tools"
    enabled = True

    def __init__(self, context: CabbageContext):
        self.context = context

    def register(self, runtime) -> dict:
        from .engine_tools import register_engine_loaders
        from Quasar.ai_modules.integrated.stream_handler import set_global_assets_summarizer
        from .asset_summary import summarize_global_assets

        tool_registry = runtime.get_registry("tool")
        set_global_assets_summarizer(summarize_global_assets)
        runtime.register_tool_loader_registrar(register_engine_loaders)
        register_engine_loaders(tool_registry)
        setattr(tool_registry, "_discovered", False)
        runtime.metadata.setdefault("cabbage_adapter", {})[self.name] = True
        logger.debug("[cai_extensions] engine loaders registered")
        return {"name": self.name}


class CabbageWorkflowSyncPlugin:
    name = "cabbage.workflow_sync"
    enabled = True

    def __init__(self, context: CabbageContext):
        self.context = context

    def register(self, runtime) -> dict:
        # 网络同步隔离能力依赖 Quasar 新增的 register_workflow_execution_scope_factory。
        # 当部署的 Quasar 版本较旧（缺该符号）时，仅降级关闭本能力，绝不能让整个 AITool
        # 插件 import 失败——否则 main.py:44 的 _install_cai_extensions 会抛异常，AITool 类
        # 永远定义不出来，LANChat start_room 报 "cannot import name 'AITool'"，AI 助手与
        # 联机 demo 全部不可用。
        try:
            from Quasar.ai_workflow import register_workflow_execution_scope_factory
            from runtime import network_sync_policy
        except Exception as exc:
            logger.warning(
                "[cai_extensions] workflow sync scope unavailable (Quasar 版本缺 "
                "register_workflow_execution_scope_factory)，已降级跳过: %s",
                exc,
            )
            runtime.metadata.setdefault("cabbage_adapter", {})[self.name] = False
            return {"name": self.name, "enabled": False}

        def create_scope(workflow_context):
            return network_sync_policy.deferred_actor_broadcasts(
                pause_engine_sync=True,
                transaction_key=workflow_context.session_id,
            )

        register_workflow_execution_scope_factory(create_scope)
        runtime.metadata.setdefault("cabbage_adapter", {})[self.name] = True
        logger.debug("[cai_extensions] workflow sync scope installed")
        return {"name": self.name}

    def shutdown(self, runtime) -> None:
        try:
            from Quasar.ai_workflow import clear_workflow_execution_scope_factory

            clear_workflow_execution_scope_factory()
        except Exception as exc:  # noqa: BLE001
            logger.debug("[cai_extensions] workflow sync scope cleanup skipped: %s", exc)


class CabbageEngineModulesPlugin:
    name = "cabbage.engine_modules"
    enabled = True

    targets = {
        "mcp": (
            ".mcp.configs.settings",
            ".mcp.tools.scene_tools",
        ),
        "scene_placement": (
            ".scene_placement.configs.settings",
            ".scene_placement.tools.loader",
        ),
    }

    def __init__(self, context: CabbageContext, modules: Iterable[str] = ("mcp", "scene_placement")):
        self.context = context
        self.modules = tuple(modules)

    def register(self, runtime) -> dict:
        imported: list[str] = []
        for name in self.modules:
            for module_name in self.targets.get(name, ()):
                try:
                    import_module(module_name, __package__)
                    imported.append(module_name)
                except Exception as exc:
                    logger.debug(
                        "[cai_extensions] preload %s failed (ok if not exists): %s",
                        module_name,
                        exc,
                    )
        runtime.metadata.setdefault("cabbage_adapter", {})[self.name] = imported
        return {"name": self.name, "imported": imported}


_INSTALL_LOCK = threading.Lock()
_INSTALLED_KEYS: set[int] = set()


def create_plugins(context: CabbageContext | None = None):
    context = bootstrap_paths(context)
    return [
        CabbagePathsPlugin(context),
        CabbageAppConfigPlugin(context),
        CabbageEngineToolsPlugin(context),
        CabbageWorkflowSyncPlugin(context),
        CabbageEngineModulesPlugin(context),
    ]


def install(app=None, context: CabbageContext | None = None) -> None:
    """Install CabbageEditor host capabilities into a CAI app/runtime."""
    context = bootstrap_paths(context)
    if app is None:
        from Quasar.cai import get_default_app

        app = get_default_app()

    install_key = id(app)
    if install_key in _INSTALLED_KEYS:
        return

    with _INSTALL_LOCK:
        if install_key in _INSTALLED_KEYS:
            return
        for plugin in create_plugins(context):
            app.register_plugin(plugin)
        _INSTALLED_KEYS.add(install_key)
        logger.info("[cai_extensions] install() complete")
