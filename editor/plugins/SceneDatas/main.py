"""Explicit legacy registration shell for the historical SceneDatas service."""

from runtime.plugin_base import PluginBase


@PluginBase.register_web("SceneDatas")
class SceneDatas(PluginBase):
    """Compatibility-only service; scene state belongs to native aggregates."""

__all__ = ["SceneDatas"]
