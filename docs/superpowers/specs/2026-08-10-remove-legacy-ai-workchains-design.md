# 删除旧 AI 工作链设计

## 目标

当前 AITool 只保留三类能力：

1. 对话答疑；
2. 节点逻辑生成与修改；
3. AgentRuntime 驱动的完整 AI 游戏/场景生成。

删除不再使用的旧场景规划和中间 `scene.json` 布置链路，避免旧工具继续被注册或误调用。

## 删除范围

- 删除 `cai_extensions/scene_plan_tools.py` 及 `generate_scene_plan` 注册入口；
- 删除整个 `cai_extensions/scene_placement/`，包括下载模型、生成中间场景 JSON、修改中间场景变换的工具和配置；
- 删除 `cai_extensions/mcp/tools/place_object_near.py` 及其注册入口；
- 删除只服务于上述链路的测试、文档、配置和路径引用。

## 保留边界

- 保留 `set_actor_transform`，因为 AgentRuntime 的布局变换 Provider 依赖它；
- 保留原生场景查询、快照、模型/环境导入和场景审查工具；
- 保留 Runtime 的规划、资源准备、布局、审查和提交工具；
- 对话服务、节点逻辑服务及其前端指导代码不做改动。

## 实现与验证

先增加工具注册边界测试，要求被删除工具不出现在引擎 loader 和 Runtime 可用工具集合中，同时要求 Runtime 必需工具仍存在。随后删除代码和引用，并运行边界测试、AITool 核心测试及 Python 编译检查。

## 风险与处理

旧工具名属于对外工具注册接口，删除后旧提示词或外部调用将不再可用；这是本次清理的预期破坏性变更。Runtime 依赖的原生工具不删除，以确保完整生成链路继续工作。
