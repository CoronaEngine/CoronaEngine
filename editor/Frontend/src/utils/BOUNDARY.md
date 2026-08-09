# Frontend utils 边界

`src/utils` 只承载无领域归属的低延迟输入 adapter、视图辅助工具，以及少量
历史兼容转发 wrapper。它不是 manifest transport、
Vue 业务 service 或场景状态的 owner。

## 文件职责与删除条件

| 文件 | 类型 | 职责 | 删除/迁移条件 |
|---|---|---|---|
| `cameraDragRegions.js` | active adapter | 计算窗口拖拽区域和排除区域 | 被统一窗口输入 adapter 替代并完成拖拽回归 |
| `constants.js` | active constants | 跨视图共享的稳定 UI 常量 | 所有调用方迁移到明确的领域常量 owner 后删除 |
| `eventBus.js` | active event adapter | 同一 JS 上下文内的 C++ 事件转发与订阅 | 公共事件 adapter 替代并完成跨 Tab/C++ 回归 |
| `panelWindows.js` | active UI adapter | 浮动面板窗口操作队列和布局辅助 | 窗口 transport/service 完整吸收后删除 |
| `serviceInitialization.js` | active lifecycle helper | 服务初始化响应错误和有限重试策略 | 统一 service lifecycle owner 吸收后删除 |
| `viewportGizmo.js` | active viewport adapter | Gizmo 命中、拖拽、取消和完成状态机 | 视口控制器集中管理且完成 pointer 回归 |
| `viewportPick.js` | active viewport adapter | 视口拾取结果和 Actor handle 索引 | 视口控制器集中管理且完成拾取回归 |
| `viewportUiMode.js` | active viewport adapter | 视口模式、光标设置和本地 UI 状态 | 视口状态 owner 吸收且完成设置迁移 |
| `bridge.js` | compatibility wrapper | 历史 bridge 导出 barrel，只 re-export `src/api` 和 `src/services` | 外部旧宿主迁移并完成兼容回归后移除 |
| `legacyEditorAdapter.js` | compatibility wrapper | 历史 raw CEF adapter 的旧 import 路径转发 | 外部插件无旧 import 后移除 |

## 依赖方向

- active utils 可以被 Vue 视图、视口控制器和对应 service 使用，但不得拥有
  Actor、Scene、项目或服务领域状态；
- active utils 不得直接调用 `window.cefQuery`，也不得导入 `utils/bridge.js`；
- `src/api/editorApi.js` 是 manifest transport 和公共 JS contract 的唯一 owner；
  `src/services` 是领域 facade 的 owner；
- `bridge.js` 只能保留兼容 re-export，不得复制请求格式、schema 或事件协议；
- raw CEF 实现集中在 `src/compat`。`src/utils/legacyEditorAdapter.js` 仅保留旧
  import 路径，不得重新实现 CEF 协议；
- 新功能不得继续放入本目录：需要跨层契约的功能进入 `src/api`，领域编排进入
  `src/services`，旧宿主协议进入 `src/compat`。

## 验证与删除策略

边界由 `editor/Frontend/tests/python/test_frontend_utils_boundary.py` 验证。删除
compatibility wrapper 前必须先确认仓内和外部宿主调用方已迁移，并保留可回退的
兼容回归证据；仅凭仓内搜索不到调用方不能直接删除。
