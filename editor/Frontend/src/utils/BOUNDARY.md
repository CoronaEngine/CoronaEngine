# Frontend utils 边界

`src/utils` 只承载无领域归属的低延迟输入 adapter 和视图辅助工具。它不是 manifest transport、
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

## 依赖方向

- active utils 可以被 Vue 视图、视口控制器和对应 service 使用，但不得拥有
  Actor、Scene、项目或服务领域状态；
- active utils 不得直接调用 `window.cefQuery`，也不得重新引入已删除的 bridge；
- `src/api/editorApi.js` 是 manifest transport 和公共 JS contract 的唯一 owner；
  `src/services` 是领域 facade 的 owner；
- raw CEF 不再由 canonical Frontend 启动入口安装；新的跨层调用必须使用
  `src/api/editorApi.js`，不得在 `src/utils` 重新引入旧 CEF wrapper；
- 新功能不得继续放入本目录：需要跨层契约的功能进入 `src/api`，领域编排进入
  `src/services`；旧宿主历史 alias 不再受支持。

## 验证与删除策略

边界由 `editor/Frontend/tests/python/test_frontend_utils_boundary.py` 验证。旧宿主
兼容入口已删除；新增跨层能力必须进入 `src/api` 或明确的领域 service。
