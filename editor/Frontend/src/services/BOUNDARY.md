# Frontend Services 边界

`src/services` 是前端 active domain facade 和 UI 编排目录，不是 C++ manifest/schema owner，
也不直接持有 Scene/Actor、AI runtime、API key 或 raw CEF transport。公共契约唯一来源仍是
`src/api/editorApi.js`。已迁移的 service 由本目录直接承载，外部旧宿主 alias 不再受支持。

## 分类

### Manifest facade

这些 active 文件提供窗口、LANChat、网络、AI、资源和项目生命周期编排；新页面应优先直接
使用对应的 `editorApi` namespace：

`lanChatService.js`、`networkService.js`、`aiService.js`、
`projectLauncherService.js`、`resourceService.js`。

`sceneService.js`、`projectService.js`、`scriptingService.js`、`fileService.js`、
`projectSettingsService.js` 和 `logService.js` 均已迁移为 canonical service owner，
直接组合 `editorApi`，不再通过兼容实现目录转发。

`appService.js` 是 Dock、CameraView 窗口和进程操作的 canonical service owner，底层调用
仍统一经过 `src/api/editorApi.js` 的 `Bridge`/`editorApi`。

`logService.js` 仅保留历史 disabled no-op 语义，但实现归 active service owner；Active Vue
页面不应调用它。

### 节点图领域编排

`nodeGraphGenerationService.js`、`nodeGraphReviewService.js`、
`nodeGraphRuntimeService.js` 负责 Blockly/节点图请求生命周期、保存、轮询、事件和结果
协调。它们可以组合 `aiService` 等 facade，但不能成为 manifest contract 或引擎状态 owner。

### Cabbage UI 状态

`cabbageAssistantContextService.js`、`cabbageGuidanceService.js`、
`cabbageTutorialSessionService.js` 负责面板上下文、引导和教程会话；它们不定义 AI 密钥、
Agent runtime、Scene/Actor 权威状态或公共引擎 API。

## 依赖约束

- service 只能通过 `src/api/editorApi.js` 或注入的 UI/store 能力访问跨层功能；
- service 不得导入已删除的 `src/utils/bridge.js`，不得使用 `window.cefQuery`；
- facade 不得复制 manifest 参数校验、错误码、revision、事件协议或状态机；
- 节点图和 Cabbage service 的本地事件/存储只服务 UI 协调，不得变成第二套引擎事实；
- 新服务必须先登记职责、canonical API owner、生命周期和删除/迁移条件，再加入本目录。

## 删除条件

删除任一兼容 facade 前必须确认外部宿主和旧面板完成迁移，并通过对应的 JS/Python 边界
测试；删除节点图或 Cabbage service 前必须先迁移所有组件调用方和持久化/事件回归。任何
删除都不能改变 `editorApi` manifest contract。
