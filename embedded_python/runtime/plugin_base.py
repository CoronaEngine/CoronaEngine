"""Shared base for explicitly registered Python plugins."""


class PluginBase:
    module_name = ""

    @classmethod
    def register_web(cls, module_name: str):
        """Mark a plugin class without creating an RPC surface."""

        def decorator(c_cls):
            c_cls.module_name = module_name
            return c_cls

        return decorator
