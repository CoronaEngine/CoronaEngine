"""Shared compatibility base for explicitly registered Python plugins."""


class PluginBase:
    module_name = ""

    @classmethod
    def register_web(cls, module_name: str):
        """Retain the historical decorator without registering Python RPC."""

        def decorator(c_cls):
            c_cls.module_name = module_name
            return c_cls

        return decorator
