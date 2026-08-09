import importlib
import logging

logger = logging.getLogger(__name__)


def reimport():
    """
    注册显式 Python 脚本服务。

    不再扫描 editor/plugins/*/main.py；脚本服务不按 Vue 页面拆分。
    """
    try:
        registry = importlib.import_module("runtime.registry")
        registered = registry.register_active_python_script_services()
        logger.info("Registered Python script services: %s", ", ".join(registered))
    except Exception as e:
        logger.error("注册 Python 脚本服务失败: %s", e, exc_info=True)
