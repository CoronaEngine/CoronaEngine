# Frontend Blockly 边界

`src/blockly` 是编辑器端 Blockly/Scratch 和节点图 UI 的 active 实现目录。
它负责积木编辑、序列化、Python 生成器输出的受限 Script Runtime 代码生成，以及节点图在前端的
校验和窗口间协调；它不是 C++ transport、编辑器公共 API 或 AI runtime 的 owner。

## 文件职责与删除条件

| 路径 | 类型 | 职责 | 删除/迁移条件 |
|---|---|---|---|
| `blocks/` | active schema | 注册自定义积木定义、字段和 UI 约束 | Blockly schema 迁移到新的图编辑器且旧 workspace 已完成兼容读取 |
| `generators/` | active compiler adapter | 将可见 Blockly workspace 编译为受限 Script Runtime Python，并组装 `CoronaEngine.*` 运行时调用 | 新编译器完整替代、生成脚本回归和旧 workspace 回归通过 |
| `components/` | active UI | 工作区 UI、Blockly workspace、工具箱、节点图和编辑器交互 | 新 UI 完整替代并完成工作区保存/运行回归 |
| `composables/` | active UI state adapter | 为 Blockly UI 提供可复用的 Actor/目标上下文行为 | 由明确的 context owner 吸收并完成跨窗口回归 |
| `configs/` | active presentation config | 工具箱、主题、分类和 workspace 配置 | 新编辑器配置 owner 吸收后删除 |
| `i18n/` | active presentation data | Blockly 自定义积木和界面文案映射 | 文案全部归统一 i18n catalog 且完成渲染回归 |
| `node-editor/aiNodeGraphService.js` | active internal adapter | 校验内部 AI 节点图结果、生成/应用请求，并通过 `BroadcastChannel` 协调 detached Dock | 节点图 adapter 被独立且等价的 UI integration owner 替代 |
| `store/` | active UI state | Blockly workspace 引用和面板 UI 状态 | 组件级状态或统一 store 完整吸收后删除 |
| `utils/` | active helper | Actor context 规范化和节点图保存重试策略 | 对应 context/save owner 吸收并完成回归 |

## 依赖方向

- `blocks/` 定义可编辑 schema，`generators/` 是唯一的 Blockly 到 Python 编译边界；
- 生成器输出属于受限 Script Runtime 命名空间，不得直接生成编辑器公共 API、
  C++ 原生对象访问或 Vue service 调用；
- `components/` 通过 `src/api/editorApi.js` 和对应 service 使用保存、运行、项目
  与 AI 请求，不得调用 `window.cefQuery` 或 `utils/bridge.js`；
- `node-editor/aiNodeGraphService.js` 只负责内部节点图 JSON 的校验、跨窗口协调和
  应用 adapter，不持有 API key、Agent runtime、C++ transport 或 Scene 权威状态；
- 节点图领域编排仍归 `src/services/nodeGraph*.js`，公共契约归 `src/api`，Script
  Runtime 的执行权限和生命周期归 Python `editor/script_runtime`；
- 新的 AI/业务逻辑不得因为调用方是 Blockly 就放入本目录，应进入相应 service
  或 Python runtime owner。

## 验证与删除策略

边界由 `editor/Frontend/tests/python/test_frontend_blockly_boundary.py` 以及现有
Frontend Node 单元测试验证。删除或移动目录前必须覆盖 Blockly workspace 序列化、
生成代码、节点图保存/运行、detached Dock 和旧项目兼容回归；仅凭仓内 import 搜索
不到调用方不能直接删除。
