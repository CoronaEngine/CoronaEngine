from runtime.plugin_base import PluginBase


@PluginBase.register_web("SceneDatas")
class SceneDatas(PluginBase):
    """Scene data web module; interactive file selection is owned by C++."""
