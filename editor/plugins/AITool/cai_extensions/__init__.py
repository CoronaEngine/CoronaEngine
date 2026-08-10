"""CabbageEditor 侧的 CAI 扩展入口包。

包含编辑器/引擎相关的能力实现，启动时通过 :func:`register.install` 注入到
Quasar（一个通用 AI 库）中。

子模块：
- ``paths_provider``：实现 CAI 路径解析器，转发到 runtime project context 和
  ``editor/config/paths_config``。
- ``app_config_provider``：实现 CAI app_config provider，转发到 ``config.app_config``。
- ``engine_tools``：注册编辑器 / 引擎相关的 MCP loaders。
- ``flows``：从 CAI 迁出的多步 LangGraph 工作流（场景合成、模型检索等）。
- ``mcp``：从 CAI 迁出的 MCP 工具集（场景、相机、模型导入等）。
- ``register``：``install()`` 入口，AITool 插件加载时调用。
"""
