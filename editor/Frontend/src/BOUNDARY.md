# Frontend `src` 目录边界

`editor/Frontend/src` 是 Vue 编辑器的宿主层。目录之间按“公共契约 → 领域 service →
页面和路由视图/组件”和“低延迟输入 adapter”分工；Vue 层不持有 C++ Scene/Actor
权威状态，不执行 Python AI 或角色脚本。

## 顶层目录职责

| 目录 | 类型 | 唯一职责 | 删除/迁移条件 |
|---|---|---|---|
| `api/` | active contract adapter | 将 C++ manifest/schema 转成 JS 公共聚合接口；唯一 transport owner；见 [`api/BOUNDARY.md`](api/BOUNDARY.md) | 不删除公共契约；单个方法迁移需有 manifest、caller 和回归证据 |
| `assets/` | static assets | 前端静态图片、样式和运行时资源 | 所有引用迁移且构建产物回归通过 |
| `blockly/` | active editor/compiler | Blockly/Scratch、节点图 UI、受限 Script Runtime 代码生成 | 见 [`blockly/BOUNDARY.md`](blockly/BOUNDARY.md) |
| `components/` | active UI | 可复用 Dock、面板和通用 UI 组件；不反向依赖路由级页面 | 调用方迁移到新 UI 且完成视觉/交互回归 |
| `composables/` | active UI state | 跨组件可复用的 Vue composition 行为 | 对应状态/行为由明确 owner 吸收后删除 |
| `config/` | active configuration | 插件面板元数据、应用和静态前端配置；不得静态导入 Vue 组件 | 配置 owner 合并且构建/启动回归通过 |
| `i18n/` | active presentation | 翻译、语言同步和 DOM 文案适配 | 统一 catalog 完整吸收并通过 i18n 检查 |
| `router/` | active navigation | 路由、页面入口和 standalone Dock 参数 | 路由系统替代且深链/独立窗口回归通过 |
| `services/` | active facade/orchestration | 领域 service、节点图和 Cabbage UI 编排；不拥有公共 transport；见 [`services/BOUNDARY.md`](services/BOUNDARY.md) | 见 [`services/BOUNDARY.md`](services/BOUNDARY.md) |
| `stores/` | active UI state | Dock、应用级 UI 状态和跨组件展示状态 | 统一状态 owner 替代且多窗口回归通过 |
| `utils/` | active adapter/compat | 低延迟输入、视口辅助和历史 bridge wrapper；见 [`utils/BOUNDARY.md`](utils/BOUNDARY.md) | 见 [`utils/BOUNDARY.md`](utils/BOUNDARY.md) |
| `views/` | active page composition | 路由级页面、侧栏、工具视图和 `panelRegistry.js` 页面组件注册；组合 API/service/component | 页面拆分或新宿主替代并完成完整交互回归 |

## 依赖方向与兼容入口

- `api/editorApi.js` 是唯一跨层 transport 和公共 JS contract adapter；
- `config/pluginManifest.js` 只提供面板元数据；`views/panelRegistry.js` 负责将
  manifest ID 映射到 Vue 组件，配置层不得反向依赖 `views/` 或具体 `.vue` 文件；
- `services/` 组合 `api/`，`views/` 组合 `api/`、`services/`、`components/` 和已登记
  的 `utils/` adapter；`components/` 不应反向依赖路由级 `views/`；
- `utils/bridge.js` 仅为外部旧宿主保留历史 re-export；transport 和 service 实现分别归
  `api/` 与 `services/`，不得在 Vue 业务中复制旧协议；
- `window.coronaBridge` 的 `cameraMove`、拾取、Gizmo、视口和输入快速通道，只能在
  `utils/viewport*.js`、视口控制器或明确登记的输入组件中使用；它不构成第二套公共
  编辑器 API，也不得传递 `Camera`、`Actor` 或 `Geometry` 对象；
- 新的场景、项目、Actor、AI 或脚本语义必须登记到公共 manifest/service owner，不能
  直接写入页面、组件、store 或通用 utils。

## 删除条件与验证

目录边界由 `editor/Frontend/tests/python/test_frontend_source_tree_boundary.py` 和
各子目录边界测试共同验证。删除目录或兼容入口前必须确认仓内与外部宿主调用方、
运行时行为、持久化和多窗口回归；仅凭搜索不到调用方不能直接删除。
